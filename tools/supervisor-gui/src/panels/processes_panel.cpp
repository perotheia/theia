// Processes panel — observer_pro_wx.erl analogue.
//
// Two-level htop-style wxTreeListCtrl. Top-level rows are workers
// (ChildState kind=0) from the latest per-machine TreeSnapshot; each
// process row expands to one child row per thread (ThreadSample on the
// wire). Resource columns come straight off the wire — the supervisor
// on the remote machine is the only thing that touches /proc; the GUI
// just renders.
//
// Process columns (in display order):
//   name | pid | parent_sup | machine | state | uptime | restarts
//   | last_exit | cpu% | rss_kb | shared_kb | data_kb | vsz_kb | threads
//
// Thread rows reuse the same column layout where it overlaps and
// surface per-thread fields in otherwise-unused columns:
//   name      → "tid=<tid> <comm>"
//   pid       → tid
//   parent    → policy (OTHER/FIFO/RR/BATCH/IDLE/DEADLINE)
//   machine   → "prio=<rt_prio> nice=<nice>"
//   state     → "cpu=<n>"            (last_ran_on_cpu)
//   uptime    → ""                   (not per-thread)
//   restarts  → ""
//   last_exit → ""
//   cpu%      → per-thread cpu%
//   rss_kb..  → ""                   (no per-thread memory split)
//   threads   → "0x<mask>"           (cpu_affinity_mask)

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/treelist.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

const char* state_name(uint32_t s) {
    switch (s) {
        case 0: return "stopped";
        case 1: return "starting";
        case 2: return "running";
        case 3: return "terminating";
        case 4: return "restarting";
        default: return "?";
    }
}

const char* policy_name(uint32_t p) {
    switch (p) {
        case 0: return "OTHER";
        case 1: return "FIFO";
        case 2: return "RR";
        case 3: return "BATCH";
        case 5: return "IDLE";
        case 6: return "DEADLINE";
        default: return "?";
    }
}

}  // namespace

// ProcessSampler placeholder — class is forward-declared in panels.h
// for ABI continuity; nothing else uses it now. Defined as an empty
// type so the unique_ptr<ProcessSampler> member can still link without
// adding a destructor.
class ProcessSampler {
public:
    ProcessSampler() = default;
};

ProcessesPanel::ProcessesPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    list_ = new wxTreeListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxTL_DEFAULT_STYLE);
    // Column 0 is the tree column (always wxALIGN_LEFT).
    list_->AppendColumn("name",       180);
    list_->AppendColumn("pid/tid",     80, wxALIGN_RIGHT);
    list_->AppendColumn("parent/policy",140);
    list_->AppendColumn("machine/sched",140);
    list_->AppendColumn("state/cpu",   100);
    list_->AppendColumn("uptime",      100, wxALIGN_RIGHT);
    list_->AppendColumn("restarts",     80, wxALIGN_RIGHT);
    list_->AppendColumn("last_exit",    90, wxALIGN_RIGHT);
    list_->AppendColumn("cpu%",         70, wxALIGN_RIGHT);
    list_->AppendColumn("rss_kb",      100, wxALIGN_RIGHT);
    list_->AppendColumn("shared_kb",   100, wxALIGN_RIGHT);
    list_->AppendColumn("data_kb",     100, wxALIGN_RIGHT);
    list_->AppendColumn("vsz_kb",      100, wxALIGN_RIGHT);
    list_->AppendColumn("threads/mask",120, wxALIGN_RIGHT);

    sizer->Add(list_, 1, wxEXPAND | wxALL, 4);
    SetSizer(sizer);
}

ProcessesPanel::~ProcessesPanel() = default;

void ProcessesPanel::on_frame(const std::string& machine_name,
                              uint16_t tag,
                              const std::string& payload) {
    if (tag != 0x0003) return;   // only TreeSnapshot
    system_supervisor::TreeSnapshot snap;
    if (!snap.ParseFromString(payload)) return;

    // Rebuild this machine's rows. Multi-machine: rows from other
    // machines stay intact; we'll see their snapshots on their own
    // TcpClient threads.
    rows_by_machine_[machine_name].clear();
    auto& v = rows_by_machine_[machine_name];
    for (const auto& c : snap.children()) {
        if (c.kind() != 0) continue;            // skip supervisors
        ProcessRow r;
        r.machine        = machine_name;
        r.name           = c.name();
        r.parent_name    = c.parent_name();
        r.pid            = c.pid();
        r.state          = c.state();
        r.restart_count  = c.restart_count();
        r.last_exit_code = c.last_exit_code();
        r.uptime_ms      = c.uptime_ms();
        r.cpu_pct        = c.cpu_pct();
        r.rss_kb         = c.rss_kb();
        r.shared_kb      = c.shared_kb();
        r.data_kb        = c.data_kb();
        r.vsz_kb         = c.vsz_kb();
        r.threads        = c.threads();
        for (const auto& t : c.threads_detail()) {
            ThreadRow tr;
            tr.tid               = t.tid();
            tr.comm              = t.comm();
            tr.cpu_pct           = t.cpu_pct();
            tr.sched_policy      = t.sched_policy();
            tr.sched_priority    = t.sched_priority();
            tr.nice              = t.nice();
            tr.cpu_affinity_mask = t.cpu_affinity_mask();
            tr.last_cpu          = t.last_cpu();
            r.thread_rows.push_back(std::move(tr));
        }
        v.push_back(std::move(r));
    }
    refresh_list();
}

void ProcessesPanel::on_sampler_update(wxThreadEvent&) {
    // No-op: kept for ABI compatibility with the now-empty sampler
    // forward declaration. Real refresh happens in on_frame.
}

void ProcessesPanel::refresh_list() {
    // Collect into a flat vector + stable sort by (machine, name).
    std::vector<ProcessRow> rows;
    for (auto& kv : rows_by_machine_) {
        for (auto& r : kv.second) rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const ProcessRow& a, const ProcessRow& b) {
                  if (a.machine != b.machine) return a.machine < b.machine;
                  return a.name < b.name;
              });

    // Remember which process rows were expanded, by (machine, name), so
    // a 1Hz refresh doesn't collapse the user's open subtrees.
    std::map<std::pair<std::string, std::string>, bool> expanded;
    for (auto item = list_->GetFirstItem(); item.IsOk();
         item = list_->GetNextSibling(item)) {
        // Tree column holds the worker name; machine is column 3 from
        // the parent row only.
        auto name    = std::string(list_->GetItemText(item, 0).ToUTF8());
        auto machine = std::string(list_->GetItemText(item, 3).ToUTF8());
        expanded[{machine, name}] = list_->IsExpanded(item);
    }

    list_->Freeze();
    list_->DeleteAllItems();
    auto root = list_->GetRootItem();
    for (const ProcessRow& r : rows) {
        auto item = list_->AppendItem(root, r.name);
        list_->SetItemText(item,  1, wxString::Format("%d", r.pid));
        list_->SetItemText(item,  2, r.parent_name);
        list_->SetItemText(item,  3, r.machine);
        list_->SetItemText(item,  4, state_name(r.state));
        list_->SetItemText(item,  5, wxString::Format("%llu",
            static_cast<unsigned long long>(r.uptime_ms / 1000)));
        list_->SetItemText(item,  6, wxString::Format("%u", r.restart_count));
        list_->SetItemText(item,  7, wxString::Format("%d", r.last_exit_code));
        list_->SetItemText(item,  8, wxString::Format("%.2f",
            static_cast<double>(r.cpu_pct) / 100.0));
        list_->SetItemText(item,  9, wxString::Format("%llu",
            static_cast<unsigned long long>(r.rss_kb)));
        list_->SetItemText(item, 10, wxString::Format("%llu",
            static_cast<unsigned long long>(r.shared_kb)));
        list_->SetItemText(item, 11, wxString::Format("%llu",
            static_cast<unsigned long long>(r.data_kb)));
        list_->SetItemText(item, 12, wxString::Format("%llu",
            static_cast<unsigned long long>(r.vsz_kb)));
        list_->SetItemText(item, 13, wxString::Format("%u", r.threads));

        for (const ThreadRow& t : r.thread_rows) {
            auto child = list_->AppendItem(item,
                wxString::Format("tid=%u %s", t.tid, t.comm.c_str()));
            list_->SetItemText(child,  1, wxString::Format("%u", t.tid));
            list_->SetItemText(child,  2, policy_name(t.sched_policy));
            list_->SetItemText(child,  3, wxString::Format(
                "prio=%u nice=%d", t.sched_priority, t.nice));
            list_->SetItemText(child,  4,
                wxString::Format("cpu=%u", t.last_cpu));
            // 5-7: uptime/restarts/last_exit are per-process only.
            list_->SetItemText(child,  8, wxString::Format("%.2f",
                static_cast<double>(t.cpu_pct) / 100.0));
            // 9-12: memory is per-process only.
            list_->SetItemText(child, 13, wxString::Format(
                "0x%llx", static_cast<unsigned long long>(t.cpu_affinity_mask)));
        }

        auto it = expanded.find({r.machine, r.name});
        if (it != expanded.end() && it->second) {
            list_->Expand(item);
        }
    }
    list_->Thaw();
}

}  // namespace sup_gui
