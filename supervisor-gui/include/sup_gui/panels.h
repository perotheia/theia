// OTP-observer-flavoured panels for supervisor-gui.
//
// Tab order + labels match Erlang's observer_wx.erl:
//
//   System         — observer_sys_wx.erl   (host/runtime facts, key/value)
//   Load Charts    — observer_perf_wx.erl  (CPU / memory plots over time)
//   Applications   — observer_app_wx.erl   (drawn supervisor diagram)
//   Processes      — observer_pro_wx.erl   (htop-like flat process list)
//   Trace          — observer_trace_wx.erl (scrolling event log)
//
// Each panel inherits the same wxPanel base and exposes one handler:
// MainFrame dispatches every incoming TCP frame to each panel; the
// panel filters by tag and decodes the protobuf itself.

#pragma once

#include <wx/event.h>
#include <wx/panel.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward-decl in the global namespace so wx pointer types resolve
// correctly without pulling those headers in here.
class wxListCtrl;
class wxStaticText;
class wxStaticBox;
class wxBoxSizer;
class wxStaticBoxSizer;
class wxFlexGridSizer;

namespace sup_gui {

class PanelBase : public wxPanel {
public:
    explicit PanelBase(wxWindow* parent) : wxPanel(parent) {}

    // Called by MainFrame on every incoming TCP frame.
    //
    // - ``machine_name`` is the row from machines.yaml that this frame
    //   arrived on. Multi-machine GUIs use it to segregate per-host data.
    // - ``tag`` is the wire frame type (0x0001 SupervisionEvent,
    //   0x0002 HealthBeacon, 0x0003 TreeSnapshot).
    // - ``payload`` is the protobuf bytes for that tag.
    virtual void on_frame(const std::string& machine_name,
                          uint16_t tag,
                          const std::string& payload) = 0;
};

// "System" — basic facts about the supervisor: uptime, generation,
// total workers / active workers, total restarts, total tombstones.
// Driven from HealthBeacon (tag 0x0002). One wxStaticBox per machine,
// laid out vertically.
class SystemPanel : public PanelBase {
public:
    explicit SystemPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    struct MachineRows {
        wxStaticBox*       box{nullptr};
        wxStaticBoxSizer*  sizer{nullptr};
        wxFlexGridSizer*   grid{nullptr};
        wxStaticText*      keys[6]{nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr};
        wxStaticText*      values[6]{nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr};
    };

    wxBoxSizer*                       container_{nullptr};
    std::map<std::string, MachineRows> machine_boxes_;
};

// "Load Charts" — periodic perf / health graphs. Today renders a
// placeholder; once we have per-pid sampling it plots CPU / memory.
class LoadChartsPanel : public PanelBase {
public:
    explicit LoadChartsPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;
};

class ApplicationsCanvas;  // defined in applications_panel.cpp

// "Applications" — drawn supervisor diagram. Boxes for SupervisorNode
// + WorkerNode entries, lines between parent and children. Modelled on
// observer_app_wx.erl. Multi-machine: each connected supervisor renders
// its own subtree below a caption with the machine name.
class ApplicationsPanel : public PanelBase {
public:
    explicit ApplicationsPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    ApplicationsCanvas* canvas_{nullptr};
};

class ProcessSampler;  // legacy placeholder; the sampler now lives on
                       // the supervisor side and ships cpu/rss/threads
                       // through ChildState. See processes_panel.cpp.

// One worker as it appears in the htop-style list. Filled from a
// remote ChildState — the GUI never touches /proc; the supervisor
// samples and ships these via the wire.
struct ProcessRow {
    std::string machine;
    std::string name;
    std::string parent_name;
    int         pid             = -1;
    uint32_t    state           = 0;
    uint32_t    restart_count   = 0;
    int32_t     last_exit_code  = 0;
    uint64_t    uptime_ms       = 0;
    uint32_t    cpu_pct         = 0;   // hundredths of a percent
    uint64_t    rss_kb          = 0;   // resident set size (total)
    uint64_t    shared_kb       = 0;   // shared with other processes
    uint64_t    data_kb         = 0;   // heap+bss+data — "memory used by app"
    uint64_t    vsz_kb          = 0;   // virtual size
    uint32_t    threads         = 0;
};

// "Processes" — flat htop-style list. Every row is one worker from the
// latest per-machine TreeSnapshot; the resource columns (cpu% / rss /
// vsz / threads) come directly off the wire — the GUI never reads
// /proc. Columns:
//   name / pid / parent_sup / machine / state / uptime / restarts
//   / last_exit / cpu% / rss_kb / vsz_kb / threads.
class ProcessesPanel : public PanelBase {
public:
    explicit ProcessesPanel(wxWindow* parent);
    ~ProcessesPanel() override;
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    void on_sampler_update(wxThreadEvent& evt);   // legacy, no-op now
    void refresh_list();

    // Latest rows, indexed by machine_name. Each snapshot replaces
    // that machine's vector; rows from other machines stay intact.
    std::map<std::string, std::vector<ProcessRow>> rows_by_machine_;

    ::wxListCtrl*                   list_{nullptr};
    std::unique_ptr<ProcessSampler> sampler_;
};

// "Trace" — scrolling SupervisionEvent log. Tombstone events surface
// here too (OTP observer doesn't have a dedicated tombstones tab).
class TracePanel : public PanelBase {
public:
    explicit TracePanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    ::wxListCtrl* list_{nullptr};
};

}  // namespace sup_gui
