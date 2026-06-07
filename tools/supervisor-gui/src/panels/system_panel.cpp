// System panel — observer_sys_wx.erl analogue.
//
// One wxStaticBox per machine, two-column key/value. Shows the SAME host +
// build facts `tdb info` / `rtdb info` print (GetSystemInfo, tag 0x0004) plus
// the supervisor HealthBeacon counters (tag 0x0002, when emitted). Values are
// updated in place; a box is created on the first frame for a machine.

#include "sup_gui/panels.h"

#include "supervisor.pb.h"

#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <ctime>
#include <map>

namespace sup_gui {

namespace {

wxFlexGridSizer* kv_sizer() {
    auto* g = new wxFlexGridSizer(/*rows*/ 0, /*cols*/ 2, /*vgap*/ 2, /*hgap*/ 16);
    g->AddGrowableCol(1);
    return g;
}

// ---- formatters, matching tdb_commands.py (_fmt_ram_mb/_uptime/_epoch_ms) ----

wxString fmt_ram_mb(uint64_t ram_kb) {
    if (!ram_kb) return "";
    const double mb = ram_kb / 1024.0;
    return wxString::Format("%.0f MB (%.1f GB)", mb, mb / 1024.0);
}

wxString fmt_uptime(uint64_t sec) {
    if (!sec) return "";
    const uint64_t days  = sec / 86400;
    const uint64_t hours = (sec % 86400) / 3600;
    const uint64_t mins  = (sec % 3600) / 60;
    wxString s;
    if (days)            s += wxString::Format("%llud ", (unsigned long long)days);
    if (days || hours)   s += wxString::Format("%lluh ", (unsigned long long)hours);
    s += wxString::Format("%llum", (unsigned long long)mins);
    return s;
}

wxString fmt_epoch_ms(uint64_t ts_ms) {
    if (!ts_ms) return "";
    const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d/%m/%y %H:%M:%S", &tm);
    return wxString::FromUTF8(buf);
}

}  // namespace

SystemPanel::SystemPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* hint  = new wxStaticText(this, wxID_ANY,
        "System overview — host + build + supervisor facts, one section per "
        "connected machine (the `tdb info` surface).");
    sizer->Add(hint, 0, wxALL, 8);

    container_ = new wxBoxSizer(wxVERTICAL);
    sizer->Add(container_, 1, wxEXPAND | wxALL, 4);

    SetSizer(sizer);
}

SystemPanel::MachineRows& SystemPanel::ensure_box(const std::string& machine_name) {
    auto it = machine_boxes_.find(machine_name);
    if (it != machine_boxes_.end()) return it->second;

    auto* box = new wxStaticBox(this, wxID_ANY, machine_name);
    auto* bsz = new wxStaticBoxSizer(box, wxVERTICAL);
    auto* kvs = kv_sizer();
    bsz->Add(kvs, 0, wxEXPAND | wxALL, 4);
    container_->Add(bsz, 0, wxEXPAND | wxALL, 4);

    MachineRows mr;
    mr.box = box; mr.sizer = bsz; mr.grid = kvs;

    // SystemInfo group first (the static host/build facts), then a thin gap,
    // then the HealthBeacon counters. Pre-create key/value cells (labels set
    // here, values filled on frames).
    static const char* kInfoKeys[kInfoRows] = {
        "hostname", "kernel", "os", "cpus", "ram",
        "uptime", "theia git sha", "build ts", "started",
    };
    for (int i = 0; i < kInfoRows; ++i) {
        mr.info_keys[i] = new wxStaticText(box, wxID_ANY, kInfoKeys[i]);
        mr.info_vals[i] = new wxStaticText(box, wxID_ANY, "—");
        kvs->Add(mr.info_keys[i], 0, wxALIGN_LEFT);
        kvs->Add(mr.info_vals[i], 1, wxALIGN_LEFT);
    }
    static const char* kHealthKeys[kHealthRows] = {
        "generation", "workers (active / total)", "total restarts",
        "total tombstones", "last heartbeat",
    };
    for (int i = 0; i < kHealthRows; ++i) {
        mr.health_keys[i] = new wxStaticText(box, wxID_ANY, kHealthKeys[i]);
        mr.health_vals[i] = new wxStaticText(box, wxID_ANY, "—");
        kvs->Add(mr.health_keys[i], 0, wxALIGN_LEFT);
        kvs->Add(mr.health_vals[i], 1, wxALIGN_LEFT);
    }

    auto inserted = machine_boxes_.emplace(machine_name, mr);
    Layout();
    return inserted.first->second;
}

void SystemPanel::on_frame(const std::string& machine_name,
                           uint16_t tag,
                           const std::string& payload) {
    if (tag == 0x0004) {            // SystemInfo (GetSystemInfo)
        system_supervisor::SystemInfo si;
        if (!si.ParseFromString(payload)) return;
        auto& mr = ensure_box(machine_name);
        mr.info_vals[0]->SetLabel(wxString::FromUTF8(si.hostname().c_str()));
        mr.info_vals[1]->SetLabel(wxString::FromUTF8(si.kernel().c_str()));
        mr.info_vals[2]->SetLabel(wxString::FromUTF8(si.os_pretty_name().c_str()));
        mr.info_vals[3]->SetLabel(wxString::Format("%u", si.cpu_count()));
        mr.info_vals[4]->SetLabel(fmt_ram_mb(si.total_ram_kb()));
        mr.info_vals[5]->SetLabel(fmt_uptime(si.uptime_sec()));
        mr.info_vals[6]->SetLabel(si.theia_git_sha().empty()
            ? "(unstamped)" : wxString::FromUTF8(si.theia_git_sha().c_str()));
        mr.info_vals[7]->SetLabel(si.build_timestamp().empty()
            ? "(unstamped)" : wxString::FromUTF8(si.build_timestamp().c_str()));
        mr.info_vals[8]->SetLabel(fmt_epoch_ms(si.start_timestamp_ms()));
        mr.sizer->Layout();
        return;
    }
    if (tag == 0x0002) {            // HealthBeacon (counters)
        system_supervisor::HealthBeacon hb;
        if (!hb.ParseFromString(payload)) return;
        auto& mr = ensure_box(machine_name);
        mr.health_vals[0]->SetLabel(wxString::Format("%llu",
            static_cast<unsigned long long>(hb.generation())));
        mr.health_vals[1]->SetLabel(wxString::Format("%u / %u",
            hb.active_workers(), hb.total_workers()));
        mr.health_vals[2]->SetLabel(wxString::Format("%llu",
            static_cast<unsigned long long>(hb.total_restarts())));
        mr.health_vals[3]->SetLabel(wxString::Format("%llu",
            static_cast<unsigned long long>(hb.total_tombstones())));
        mr.health_vals[4]->SetLabel(fmt_epoch_ms(hb.timestamp_ms()));
        mr.sizer->Layout();
        return;
    }
}

}  // namespace sup_gui
