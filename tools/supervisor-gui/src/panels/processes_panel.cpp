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
#include <wx/dataview.h>   // GetDataView() + wxEVT_DATAVIEW_COLUMN_HEADER_CLICK

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

    // Click a column header to sort by it (toggle asc/desc). wxTreeListCtrl's
    // own COLUMN_SORTED event only fires for its built-in LEXICAL sort (wrong
    // for numeric cols: "100" < "9", "17.2 MB" vs "4.0 MB"). Instead we bind
    // the inner wxDataViewCtrl's header-click directly (fires on every header
    // click) and run our OWN typed sort in refresh_list.
    if (auto* dv = list_->GetDataView()) {
        dv->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK,
                 &ProcessesPanel::on_col_click, this);
    }
}

void ProcessesPanel::on_col_click(wxDataViewEvent& evt) {
    const int col = evt.GetColumn();
    if (col < 0) return;
    if (col == sort_col_) sort_desc_ = !sort_desc_;   // same col → flip dir
    else { sort_col_ = col; sort_desc_ = false; }     // new col → ascending
    refresh_list();
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
    // Collect into a flat vector, then sort. Default (sort_col_ < 0): by
    // (machine, name). Otherwise by the clicked column — TYPED (numeric cols
    // compare as numbers, not lexically), direction per sort_desc_.
    std::vector<ProcessRow> rows;
    for (auto& kv : rows_by_machine_) {
        for (auto& r : kv.second) rows.push_back(r);
    }
    const int sc = sort_col_;
    const bool desc = sort_desc_;
    // Column → a typed key. Numeric cols return a double in `num`; string cols
    // set `str`. -1/unknown falls through to (machine, name).
    auto num_key = [](const ProcessRow& r, int col) -> double {
        switch (col) {
            case 1:  return r.pid;                       // pid
            case 5:  return static_cast<double>(r.uptime_ms);
            case 6:  return r.restart_count;
            case 7:  return r.last_exit_code;
            case 8:  return r.cpu_pct;                   // cpu%
            case 9:  return static_cast<double>(r.rss_kb);
            case 10: return static_cast<double>(r.shared_kb);
            case 11: return static_cast<double>(r.data_kb);
            case 12: return static_cast<double>(r.vsz_kb);
            case 13: return r.threads;
            default: return 0.0;
        }
    };
    auto is_numeric_col = [](int col) {
        return col == 1 || (col >= 5 && col <= 13);
    };
    std::stable_sort(rows.begin(), rows.end(),
        [&](const ProcessRow& a, const ProcessRow& b) {
            int cmp = 0;
            if (sc < 0) {
                if (a.machine != b.machine) cmp = a.machine < b.machine ? -1 : 1;
                else cmp = a.name.compare(b.name);
            } else if (is_numeric_col(sc)) {
                const double x = num_key(a, sc), y = num_key(b, sc);
                cmp = (x < y) ? -1 : (x > y) ? 1 : 0;
            } else {
                // string columns: 0=name, 2=parent, 3=machine.
                const std::string& x = (sc == 2) ? a.parent_name
                                     : (sc == 3) ? a.machine : a.name;
                const std::string& y = (sc == 2) ? b.parent_name
                                     : (sc == 3) ? b.machine : b.name;
                cmp = x.compare(y);
            }
            if (cmp == 0) cmp = a.name.compare(b.name);   // stable tiebreak
            return desc ? cmp > 0 : cmp < 0;
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

    // Remember the SELECTED row by (machine, name) — without this, DeleteAll
    // below drops the selection every refresh, so a 1Hz snapshot keeps clearing
    // whatever the user clicked. A selected THREAD child keys off its parent
    // process (we re-select the process row; thread identity isn't tracked).
    std::pair<std::string, std::string> sel_key;
    bool had_sel = false;
    if (auto s = list_->GetSelection(); s.IsOk()) {
        auto parent = list_->GetItemParent(s);
        // For a thread (child) row, the process row is its parent; for a
        // process row, parent is the (invisible) root, so use the row itself.
        auto proc = (parent.IsOk() && parent != list_->GetRootItem()) ? parent : s;
        sel_key = { std::string(list_->GetItemText(proc, 3).ToUTF8()),
                    std::string(list_->GetItemText(proc, 0).ToUTF8()) };
        had_sel = true;
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

        // Re-select the row the user had selected before the rebuild.
        if (had_sel && sel_key == std::make_pair(r.machine, r.name)) {
            list_->Select(item);
            list_->EnsureVisible(item);
        }
    }
    list_->Thaw();
}

}  // namespace sup_gui
