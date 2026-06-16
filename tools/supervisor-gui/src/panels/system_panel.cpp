// System panel — Erlang observer "System" tab analogue, Theia-native.
//
// Per connected machine, four sub-boxes laid out 2×2:
//   System & Architecture | Resources
//   Supervisor Statistics | GPU / Accelerators
//
// Fed by GetSystemInfo (tag 0x0004) and HealthBeacon (tag 0x0002) — the same
// surface `tdb info` prints. Values are updated in place; the boxes are created
// on the first frame seen for a machine. The GPU box is a placeholder until the
// shwa FC's nvidia-smi/jtop telemetry path is wired (a follow-up).

#include "sup_gui/panels.h"

#include "supervisor.pb.h"
#include "supervisor_bridge.pb.h"   // services::com::AccelSample (SHWA telemetry)

#include <wx/wx.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
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

// Build a titled sub-box with a 2-col key/value grid; fill `vals[]` with the
// value cells (labels come from `keys`). Returns the box's sizer so the caller
// can drop it into the 2×2 layout.
wxSizer* make_subbox(wxWindow* parent,
                     const wxString& title,
                     const char* const* keys,
                     wxStaticText** vals,
                     int n) {
    auto* box = new wxStaticBox(parent, wxID_ANY, title);
    auto* bsz = new wxStaticBoxSizer(box, wxVERTICAL);
    auto* g   = kv_sizer();
    for (int i = 0; i < n; ++i) {
        auto* k = new wxStaticText(box, wxID_ANY, keys[i]);
        vals[i] = new wxStaticText(box, wxID_ANY, "—");
        g->Add(k, 0, wxALIGN_LEFT);
        g->Add(vals[i], 1, wxALIGN_LEFT);
    }
    bsz->Add(g, 0, wxEXPAND | wxALL, 4);
    return bsz;
}

// ---- formatters, matching tdb_commands.py (_fmt_ram_mb/_uptime/_epoch_ms) ----

wxString fmt_ram_mb(uint64_t ram_kb) {
    if (!ram_kb) return "";
    const double mb = ram_kb / 1024.0;
    return wxString::Format("%.0f MB (%.1f GB)", mb, mb / 1024.0);
}

// "used / total GB (NN%)" from a (total_kb, avail_kb) statvfs pair.
wxString fmt_disk(uint64_t total_kb, uint64_t avail_kb) {
    if (!total_kb) return "(n/a)";
    const uint64_t used_kb = total_kb >= avail_kb ? total_kb - avail_kb : 0;
    const double   used_gb = used_kb  / 1024.0 / 1024.0;
    const double   tot_gb  = total_kb / 1024.0 / 1024.0;
    const int      pct     = static_cast<int>((used_kb * 100) / total_kb);
    return wxString::Format("%.1f / %.0f GB (%d%%)", used_gb, tot_gb, pct);
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

const char* const kArchKeys[]   = {
    "hostname", "kernel", "os", "logical CPUs", "theia git sha", "build ts",
};
const char* const kResKeys[]    = {
    "RAM total", "disk /", "disk install", "host uptime", "supervisor started",
};
const char* const kHealthKeys[] = {
    "generation", "workers (active / total)", "total restarts",
    "total tombstones", "last heartbeat",
};
const char* const kGpuKeys[]    = {
    "board", "GPU util", "GPU freq", "temp", "power", "fan",
};

}  // namespace

SystemPanel::SystemPanel(wxWindow* parent) : PanelBase(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* hint  = new wxStaticText(this, wxID_ANY,
        "System overview — host, resources, supervisor stats and accelerators, "
        "one section per connected machine (the `tdb info` surface).");
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

    MachineRows mr;
    mr.box = box; mr.sizer = bsz;

    // 2×2 grid of sub-boxes (observer System-tab layout). Both columns grow so
    // the boxes share width evenly.
    auto* grid = new wxFlexGridSizer(/*rows*/ 2, /*cols*/ 2, /*vgap*/ 6, /*hgap*/ 8);
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);

    grid->Add(make_subbox(box, "System & Architecture",
                          kArchKeys, mr.arch_vals, kArchRows),
              1, wxEXPAND);
    grid->Add(make_subbox(box, "Resources",
                          kResKeys, mr.res_vals, kResRows),
              1, wxEXPAND);
    grid->Add(make_subbox(box, "Supervisor Statistics",
                          kHealthKeys, mr.health_vals, kHealthRows),
              1, wxEXPAND);

    // GPU / Accelerators — filled live from SHWA AccelSample (tag 0x0006).
    grid->Add(make_subbox(box, "GPU / Accelerators",
                          kGpuKeys, mr.gpu_vals, kGpuRows),
              1, wxEXPAND);

    bsz->Add(grid, 1, wxEXPAND | wxALL, 4);
    container_->Add(bsz, 0, wxEXPAND | wxALL, 4);

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

        // System & Architecture — supervisor IDENTITY (SystemInfo carries this
        // only now; the host hardware stats moved to SHWA / the AccelSample arm).
        mr.arch_vals[0]->SetLabel(wxString::FromUTF8(si.hostname().c_str()));
        mr.arch_vals[1]->SetLabel(wxString::FromUTF8(si.kernel().c_str()));
        mr.arch_vals[2]->SetLabel(wxString::FromUTF8(si.os_pretty_name().c_str()));
        // arch_vals[3] (CPU count) is filled from AccelSample (SHWA) below.
        mr.arch_vals[4]->SetLabel(si.theia_git_sha().empty()
            ? "(unstamped)" : wxString::FromUTF8(si.theia_git_sha().c_str()));
        mr.arch_vals[5]->SetLabel(si.build_timestamp().empty()
            ? "(unstamped)" : wxString::FromUTF8(si.build_timestamp().c_str()));

        // Resources — only "supervisor started" comes from SystemInfo now; RAM /
        // disk / uptime are filled from the SHWA AccelSample arm (tag 0x0006).
        mr.res_vals[4]->SetLabel(fmt_epoch_ms(si.start_timestamp_ms()));

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
    if (tag == 0x0006) {            // AccelSample (SHWA GPU / host telemetry)
        services::com::AccelSample a;
        if (!a.ParseFromString(payload)) return;
        auto& mr = ensure_box(machine_name);
        // GPU / Accelerators box: board, util, freq, temp, power, fan.
        wxString board = a.board().empty()
            ? "(unknown)" : wxString::FromUTF8(a.board().c_str());
        if (a.on_jetson()) board += " (Jetson)";
        mr.gpu_vals[0]->SetLabel(board);
        mr.gpu_vals[1]->SetLabel(wxString::Format("%u%%", a.gpu_util_pct()));
        mr.gpu_vals[2]->SetLabel(a.gpu_freq_mhz()
            ? wxString::Format("%u MHz", a.gpu_freq_mhz()) : "—");
        mr.gpu_vals[3]->SetLabel(a.temp_c()
            ? wxString::Format("%u °C", a.temp_c()) : "—");
        mr.gpu_vals[4]->SetLabel(a.power_mw()
            ? wxString::Format("%.1f W", a.power_mw() / 1000.0) : "—");
        mr.gpu_vals[5]->SetLabel(a.fan_rpm()
            ? wxString::Format("%u rpm", a.fan_rpm()) : "—");
        // SHWA is the host system-monitor (the supervisor shed these): fill the
        // Resources box's RAM / disk / uptime + the arch CPU count live.
        if (a.cpu_count())
            mr.arch_vals[3]->SetLabel(wxString::Format("%u", a.cpu_count()));
        if (a.mem_total_mb())
            mr.res_vals[0]->SetLabel(fmt_ram_mb(
                static_cast<uint64_t>(a.mem_total_mb()) * 1024));
        if (a.disk_root_total_kb())
            mr.res_vals[1]->SetLabel(fmt_disk(a.disk_root_total_kb(),
                                              a.disk_root_avail_kb()));
        if (a.disk_install_total_kb())
            mr.res_vals[2]->SetLabel(fmt_disk(a.disk_install_total_kb(),
                                              a.disk_install_avail_kb()));
        if (a.uptime_sec())
            mr.res_vals[3]->SetLabel(fmt_uptime(a.uptime_sec()));
        mr.sizer->Layout();
        return;
    }
}

}  // namespace sup_gui
