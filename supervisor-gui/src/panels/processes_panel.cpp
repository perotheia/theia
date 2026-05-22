// Processes panel — observer_pro_wx.erl analogue.
//
// Flat htop-style wxListCtrl. Every row is one worker (ChildState
// kind=0) from the latest per-machine TreeSnapshot. Resource columns
// (cpu%, rss_kb, vsz_kb, threads) come straight off the wire — the
// supervisor on the remote machine is the only thing that touches
// /proc; the GUI just renders.
//
// Columns (in display order):
//   name | pid | parent_sup | machine | state | uptime | restarts
//   | last_exit | cpu% | rss_kb | vsz_kb | threads

#include "sup_gui/panels.h"

#include "TreeSnapshot.pb.h"

#include <wx/wx.h>
#include <wx/listctrl.h>

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
    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL);
    list_->InsertColumn( 0, "name",       wxLIST_FORMAT_LEFT,   180);
    list_->InsertColumn( 1, "pid",        wxLIST_FORMAT_RIGHT,   80);
    list_->InsertColumn( 2, "parent_sup", wxLIST_FORMAT_LEFT,   140);
    list_->InsertColumn( 3, "machine",    wxLIST_FORMAT_LEFT,   120);
    list_->InsertColumn( 4, "state",      wxLIST_FORMAT_LEFT,    90);
    list_->InsertColumn( 5, "uptime",     wxLIST_FORMAT_RIGHT,  100);
    list_->InsertColumn( 6, "restarts",   wxLIST_FORMAT_RIGHT,   80);
    list_->InsertColumn( 7, "last_exit",  wxLIST_FORMAT_RIGHT,   90);
    list_->InsertColumn( 8, "cpu%",       wxLIST_FORMAT_RIGHT,   70);
    list_->InsertColumn( 9, "rss_kb",     wxLIST_FORMAT_RIGHT,  100);
    list_->InsertColumn(10, "vsz_kb",     wxLIST_FORMAT_RIGHT,  100);
    list_->InsertColumn(11, "threads",    wxLIST_FORMAT_RIGHT,   80);

    sizer->Add(list_, 1, wxEXPAND | wxALL, 4);
    SetSizer(sizer);
}

ProcessesPanel::~ProcessesPanel() = default;

void ProcessesPanel::on_frame(const std::string& machine_name,
                              uint16_t tag,
                              const std::string& payload) {
    if (tag != 0x0003) return;   // only TreeSnapshot
    services::supervisor::TreeSnapshot snap;
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
        r.vsz_kb         = c.vsz_kb();
        r.threads        = c.threads();
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

    list_->Freeze();
    list_->DeleteAllItems();
    for (size_t i = 0; i < rows.size(); ++i) {
        const ProcessRow& r = rows[i];
        long row = list_->InsertItem(static_cast<long>(i), r.name);
        list_->SetItem(row,  1, wxString::Format("%d", r.pid));
        list_->SetItem(row,  2, r.parent_name);
        list_->SetItem(row,  3, r.machine);
        list_->SetItem(row,  4, state_name(r.state));
        list_->SetItem(row,  5, wxString::Format("%llu",
            static_cast<unsigned long long>(r.uptime_ms / 1000)));
        list_->SetItem(row,  6, wxString::Format("%u", r.restart_count));
        list_->SetItem(row,  7, wxString::Format("%d", r.last_exit_code));
        // cpu% is hundredths of a percent on the wire.
        list_->SetItem(row,  8, wxString::Format("%.2f",
            static_cast<double>(r.cpu_pct) / 100.0));
        list_->SetItem(row,  9, wxString::Format("%llu",
            static_cast<unsigned long long>(r.rss_kb)));
        list_->SetItem(row, 10, wxString::Format("%llu",
            static_cast<unsigned long long>(r.vsz_kb)));
        list_->SetItem(row, 11, wxString::Format("%u", r.threads));
    }
    list_->Thaw();
}

}  // namespace sup_gui
