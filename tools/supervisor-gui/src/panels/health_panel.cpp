// Health panel — the at-a-glance rig health card. Answers "is this rig healthy,
// and which FC is crash-looping?" without reading the whole Applications tree.
//
// DATA SOURCE (no new gRPC — both feeds already arrive):
//   tag 0x0002  HealthBeacon  — cluster totals (active/total workers, restarts,
//                               tombstones, generation, supervisor uptime)
//   tag 0x0003  TreeSnapshot  — per-FC ChildState (name/state/pid/restart_count/
//                               flags/uptime). We summarise the workers.
//
// Layout: a header line with a red/amber/green dot + the cluster counts, then a
// per-FC list sorted UNHEALTHY-FIRST (core-dumped, then degraded, then restart>0,
// then by name). A high ↻N with low uptime = a crash loop, obvious at a glance.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"   // system_supervisor.{TreeSnapshot,ChildState,HealthBeacon}

#include <wx/wx.h>
#include <wx/listctrl.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

constexpr uint16_t kTagHealth   = 0x0002;
constexpr uint16_t kTagSnapshot = 0x0003;

constexpr uint32_t kFlagCoreDumped = 1u;   // bit0
constexpr uint32_t kFlagDegraded   = 2u;   // bit1

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

struct FcHealth {
    std::string name;
    uint32_t    kind{};
    int32_t     pid{};
    uint32_t    state{};
    uint32_t    restart_count{};
    uint32_t    flags{};
    uint64_t    uptime_ms{};
    uint32_t    cpu_pct{};
    uint64_t    rss_kb{};

    // Worse health → smaller rank → sorts first.
    int rank() const {
        if (flags & kFlagCoreDumped)        return 0;
        if (flags & kFlagDegraded)          return 1;
        if (pid <= 0 && kind == 0)          return 2;   // a worker not running
        if (restart_count > 0)              return 3;
        return 4;                                        // healthy
    }
};

}  // namespace

struct HealthPanelImpl {
    // Header (cluster summary).
    wxStaticText* dot{nullptr};       // ● coloured by worst health
    wxStaticText* summary{nullptr};   // "workers a/t · restarts R · tombstones T · gen G"
    // Per-FC list.
    wxListCtrl*   list{nullptr};

    // Latest beacon + tree (per machine; v1 shows the focused machine's).
    uint32_t active_workers{0}, total_workers{0};
    uint64_t total_restarts{0}, total_tombstones{0}, generation{0};
    std::vector<FcHealth> fcs;

    void repaint() {
        // ---- worst-health dot ----
        int worst = 4;
        for (const auto& f : fcs) worst = std::min(worst, f.rank());
        wxColour col(60, 200, 60);                 // green
        const char* glyph = "● healthy";
        if (worst <= 1)      { col = wxColour(220, 60, 60);  glyph = "● UNHEALTHY"; }
        else if (worst <= 3) { col = wxColour(230, 170, 40); glyph = "● degraded";  }
        // also red if the beacon says workers are down
        if (total_workers > 0 && active_workers < total_workers) {
            col = wxColour(220, 60, 60); glyph = "● workers down";
        }
        dot->SetLabel(glyph);
        dot->SetForegroundColour(col);

        summary->SetLabel(wxString::Format(
            "workers %u/%u   restarts %llu   tombstones %llu   gen %llu",
            active_workers, total_workers,
            static_cast<unsigned long long>(total_restarts),
            static_cast<unsigned long long>(total_tombstones),
            static_cast<unsigned long long>(generation)));

        // ---- per-FC rows (unhealthy first) ----
        std::vector<FcHealth> rows = fcs;
        std::sort(rows.begin(), rows.end(), [](const FcHealth& a, const FcHealth& b) {
            if (a.rank() != b.rank()) return a.rank() < b.rank();
            return a.name < b.name;
        });
        list->DeleteAllItems();
        long i = 0;
        for (const auto& f : rows) {
            long row = list->InsertItem(i++, wxString::FromUTF8(f.name.c_str()));
            wxString st = state_name(f.state);
            if (f.flags & kFlagCoreDumped) st += " ✗";
            else if (f.flags & kFlagDegraded) st += " ⚠";
            list->SetItem(row, 1, st);
            list->SetItem(row, 2, f.restart_count ? wxString::Format("↻%u", f.restart_count)
                                                  : wxString("-"));
            // uptime in a friendly unit
            uint64_t s = f.uptime_ms / 1000;
            wxString up = s >= 3600 ? wxString::Format("%lluh%llum", s/3600, (s%3600)/60)
                        : s >= 60   ? wxString::Format("%llum%llus", s/60, s%60)
                                    : wxString::Format("%llus", static_cast<unsigned long long>(s));
            list->SetItem(row, 3, f.pid > 0 ? up : wxString("-"));
            list->SetItem(row, 4, wxString::Format("%u%%", f.cpu_pct));
            list->SetItem(row, 5, f.rss_kb ? wxString::Format("%lluM", f.rss_kb/1024)
                                           : wxString("-"));
            // colour the row by health
            wxColour bg(255,255,255);
            if (f.flags & kFlagCoreDumped) bg = wxColour(255,205,210);
            else if (f.flags & kFlagDegraded) bg = wxColour(255,235,200);
            else if (f.restart_count > 0) bg = wxColour(255,248,220);
            list->SetItemBackgroundColour(row, bg);
        }
    }
};

HealthPanel::HealthPanel(wxWindow* parent) : PanelBase(parent) {
    impl_ = new HealthPanelImpl();
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* box = new wxStaticBoxSizer(wxVERTICAL, this, "Rig health");
    impl_->dot = new wxStaticText(box->GetStaticBox(), wxID_ANY, "● —");
    wxFont f = impl_->dot->GetFont(); f.SetPointSize(f.GetPointSize() + 2);
    f.SetWeight(wxFONTWEIGHT_BOLD); impl_->dot->SetFont(f);
    box->Add(impl_->dot, 0, wxALL, 4);
    impl_->summary = new wxStaticText(box->GetStaticBox(), wxID_ANY,
        "workers -/-   restarts -   tombstones -");
    box->Add(impl_->summary, 0, wxLEFT | wxBOTTOM, 6);
    sizer->Add(box, 0, wxEXPAND | wxALL, 4);

    impl_->list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxLC_REPORT | wxLC_SINGLE_SEL);
    impl_->list->AppendColumn("FC",      wxLIST_FORMAT_LEFT, 150);
    impl_->list->AppendColumn("State",   wxLIST_FORMAT_LEFT,  90);
    impl_->list->AppendColumn("Restart", wxLIST_FORMAT_LEFT,  70);
    impl_->list->AppendColumn("Uptime",  wxLIST_FORMAT_LEFT,  80);
    impl_->list->AppendColumn("CPU",     wxLIST_FORMAT_LEFT,  55);
    impl_->list->AppendColumn("RSS",     wxLIST_FORMAT_LEFT,  60);
    impl_->list->SetMinSize(wxSize(60, 40));   // floor → no negative-height alloc
    sizer->Add(impl_->list, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);
}

void HealthPanel::on_frame(const std::string& /*machine_name*/, uint16_t tag,
                           const std::string& payload) {
    if (tag == kTagHealth) {
        system_supervisor::HealthBeacon hb;
        if (!hb.ParseFromString(payload)) return;
        impl_->active_workers   = hb.active_workers();
        impl_->total_workers    = hb.total_workers();
        impl_->total_restarts   = hb.total_restarts();
        impl_->total_tombstones = hb.total_tombstones();
        impl_->generation       = hb.generation();
        impl_->repaint();
        return;
    }
    if (tag == kTagSnapshot) {
        system_supervisor::TreeSnapshot snap;
        if (!snap.ParseFromString(payload)) return;
        impl_->fcs.clear();
        for (const auto& c : snap.children()) {
            FcHealth f;
            f.name          = c.name();
            f.kind          = c.kind();
            f.pid           = c.pid();
            f.state         = c.state();
            f.restart_count = c.restart_count();
            f.flags         = c.flags();
            f.uptime_ms     = c.uptime_ms();
            f.cpu_pct       = c.cpu_pct();
            f.rss_kb        = c.rss_kb();
            // Show workers (kind 0) + nodes (kind 2); skip the supervisor rows
            // (kind 1) — they're structure, not FC health.
            if (c.kind() == 1) continue;
            impl_->fcs.push_back(std::move(f));
        }
        impl_->repaint();
        return;
    }
}

}  // namespace sup_gui
