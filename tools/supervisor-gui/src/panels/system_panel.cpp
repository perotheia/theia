// System panel — observer_sys_wx.erl analogue.
//
// Two-column key/value layout, one wxStaticBoxSizer per category. Today
// shows everything HealthBeacon delivers; once the supervisor exposes a
// SystemInfo RPC (host, kernel, cpu count, total RAM) we extend this.
//
// Multi-machine: the rows are keyed by machine_name and a wxStaticBox
// per machine groups its facts.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <chrono>
#include <map>
#include <mutex>

namespace sup_gui {

namespace {

// Two-column row helper used inside each StaticBox.
wxFlexGridSizer* kv_sizer() {
    auto* g = new wxFlexGridSizer(/*rows*/ 0, /*cols*/ 2, /*vgap*/ 2, /*hgap*/ 16);
    g->AddGrowableCol(1);
    return g;
}

void kv_row(wxFlexGridSizer* g, wxWindow* parent,
            const wxString& key, const wxString& value) {
    auto* k = new wxStaticText(parent, wxID_ANY, key);
    auto* v = new wxStaticText(parent, wxID_ANY, value);
    g->Add(k, 0, wxALIGN_LEFT);
    g->Add(v, 1, wxALIGN_LEFT);
}

}  // namespace

SystemPanel::SystemPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* hint  = new wxStaticText(this, wxID_ANY,
        "System overview. One section per connected machine.");
    sizer->Add(hint, 0, wxALL, 8);

    // Container for per-machine boxes — populated lazily as HealthBeacons
    // arrive. We attach all boxes to this sizer at runtime.
    container_ = new wxBoxSizer(wxVERTICAL);
    sizer->Add(container_, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);
}

void SystemPanel::on_frame(const std::string& machine_name,
                           uint16_t tag,
                           const std::string& payload) {
    if (tag != 0x0002) return;  // HealthBeacon
    system_supervisor::HealthBeacon hb;
    if (!hb.ParseFromString(payload)) return;

    auto it = machine_boxes_.find(machine_name);
    if (it == machine_boxes_.end()) {
        auto* box = new wxStaticBox(this, wxID_ANY, machine_name);
        auto* bsz = new wxStaticBoxSizer(box, wxVERTICAL);
        auto* kvs = kv_sizer();
        bsz->Add(kvs, 0, wxEXPAND | wxALL, 4);
        container_->Add(bsz, 0, wxEXPAND | wxALL, 4);

        MachineRows mr;
        mr.box   = box;
        mr.sizer = bsz;
        mr.grid  = kvs;
        // Six labels; values updated in place to avoid relayout churn.
        for (int i = 0; i < 6; ++i) {
            auto* k = new wxStaticText(box, wxID_ANY, "");
            auto* v = new wxStaticText(box, wxID_ANY, "");
            kvs->Add(k, 0, wxALIGN_LEFT);
            kvs->Add(v, 1, wxALIGN_LEFT);
            mr.keys[i] = k;
            mr.values[i] = v;
        }
        auto inserted = machine_boxes_.emplace(machine_name, mr);
        it = inserted.first;
        Layout();
    }

    auto& mr = it->second;
    mr.keys[0]->SetLabel("uptime (ms)");
    mr.keys[1]->SetLabel("generation");
    mr.keys[2]->SetLabel("workers (active / total)");
    mr.keys[3]->SetLabel("total restarts");
    mr.keys[4]->SetLabel("total tombstones");
    mr.keys[5]->SetLabel("last heartbeat (epoch ms)");

    mr.values[0]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(hb.uptime_ms())));
    mr.values[1]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(hb.generation())));
    mr.values[2]->SetLabel(wxString::Format("%u / %u",
        hb.active_workers(), hb.total_workers()));
    mr.values[3]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(hb.total_restarts())));
    mr.values[4]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(hb.total_tombstones())));
    mr.values[5]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(hb.timestamp_ms())));
    mr.sizer->Layout();
}

}  // namespace sup_gui
