// Perf panel — placeholder for observer_perf_wx.erl + observer_sys_wx.erl.
// TODO: CPU per core, memory, total_restarts / total_tombstones over time
// using HealthBeacon as the data source.

#include "sup_gui/panels.h"

#include "HealthBeacon.pb.h"

#include <wx/wx.h>

namespace sup_gui {

namespace {
struct PerfState {
    wxStaticText* uptime{nullptr};
    wxStaticText* workers{nullptr};
    wxStaticText* restarts{nullptr};
    wxStaticText* tombstones{nullptr};
};
PerfState* state(PerfPanel* p) {
    return reinterpret_cast<PerfState*>(p->GetClientData());
}
}  // namespace

PerfPanel::PerfPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxFlexGridSizer(4, 2, 6, 12);

    auto add_row = [&](const char* label, wxStaticText** out) {
        sizer->Add(new wxStaticText(this, wxID_ANY, label),
                   0, wxALIGN_CENTER_VERTICAL);
        *out = new wxStaticText(this, wxID_ANY, "—");
        sizer->Add(*out, 0, wxALIGN_CENTER_VERTICAL);
    };

    auto* st = new PerfState();
    add_row("uptime:",         &st->uptime);
    add_row("workers:",        &st->workers);
    add_row("total restarts:", &st->restarts);
    add_row("tombstones:",     &st->tombstones);
    SetClientData(st);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(sizer, 0, wxALL, 12);
    SetSizer(outer);
}

void PerfPanel::on_frame(uint16_t tag, const std::string& payload) {
    if (tag != 0x0002) return;  // HealthBeacon
    services::supervisor::HealthBeacon hb;
    if (!hb.ParseFromString(payload)) return;

    auto* st = state(this);
    if (!st) return;
    st->uptime    ->SetLabel(wxString::Format("%llu ms",
                                              static_cast<unsigned long long>(hb.uptime_ms())));
    st->workers   ->SetLabel(wxString::Format("%u / %u  (active / total)",
                                              hb.active_workers(), hb.total_workers()));
    st->restarts  ->SetLabel(wxString::Format("%llu",
                                              static_cast<unsigned long long>(hb.total_restarts())));
    st->tombstones->SetLabel(wxString::Format("%llu",
                                              static_cast<unsigned long long>(hb.total_tombstones())));
}

}  // namespace sup_gui
