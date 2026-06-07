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
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Forward-decl in the global namespace so wx pointer types resolve
// correctly without pulling those headers in here.
class wxListCtrl;
class wxListEvent;
class wxMouseEvent;
class wxTreeListCtrl;
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

// "System" — host + build + supervisor facts, one wxStaticBox per machine.
// Two groups, the same surface `tdb info` / `rtdb info` show:
//   - SystemInfo (tag 0x0004, GetSystemInfo): hostname, kernel, os, cpus,
//     ram (MB/GB), uptime (Dd Hh Mm), git sha, build ts, started (local).
//   - HealthBeacon (tag 0x0002): generation, workers, restarts, tombstones,
//     last heartbeat. (Empty until the supervisor emits health frames — com's
//     Subscribe is snapshot-only under the pull model; SystemInfo is the live
//     content today.)
class SystemPanel : public PanelBase {
public:
    explicit SystemPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    // SystemInfo = 9 rows; HealthBeacon = 5 rows. Pre-allocated key/value
    // wxStaticTexts updated in place to avoid relayout churn.
    static constexpr int kInfoRows   = 9;
    static constexpr int kHealthRows = 5;
    struct MachineRows {
        wxStaticBox*       box{nullptr};
        wxStaticBoxSizer*  sizer{nullptr};
        wxFlexGridSizer*   grid{nullptr};
        wxStaticText*      info_keys[kInfoRows]{};
        wxStaticText*      info_vals[kInfoRows]{};
        wxStaticText*      health_keys[kHealthRows]{};
        wxStaticText*      health_vals[kHealthRows]{};
    };

    MachineRows& ensure_box(const std::string& machine_name);

    wxBoxSizer*                       container_{nullptr};
    std::map<std::string, MachineRows> machine_boxes_;
};

// "Load Charts" — periodic perf / health graphs. Today renders a
// placeholder; once we have per-pid sampling it plots CPU / memory.
class LoadChartsCanvas;  // defined in load_charts_panel.cpp
class LoadChartsPanel : public PanelBase {
public:
    explicit LoadChartsPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    LoadChartsCanvas* canvas_{nullptr};
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

    // #365 — wire a callback the panel calls when the user clicks
    // Apply in the ConfigureTrace right-click dialog. main_frame
    // sets this to look up the matching GrpcClient and call
    // configure_trace(node, msg, enabled). Lifetime: the panel
    // captures by value, so the closure may safely outlive the call.
    using ConfigureTraceCallback = std::function<void(
        const std::string& /*machine*/, const std::string& /*node*/,
        const std::string& /*msg_type*/, bool /*enabled*/)>;
    void set_configure_trace_callback(ConfigureTraceCallback cb);

private:
    ApplicationsCanvas* canvas_{nullptr};
};

class ProcessSampler;  // legacy placeholder; the sampler now lives on
                       // the supervisor side and ships cpu/rss/threads
                       // through ChildState. See processes_panel.cpp.

// One thread as it appears under a process row in the tree.
struct ThreadRow {
    uint32_t tid               = 0;
    std::string comm;
    uint32_t cpu_pct           = 0;  // hundredths of a percent
    uint32_t sched_policy      = 0;
    uint32_t sched_priority    = 0;
    int32_t  nice              = 0;
    uint64_t cpu_affinity_mask = 0;
    uint32_t last_cpu          = 0;
};

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
    std::vector<ThreadRow> thread_rows;
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

    ::wxTreeListCtrl*               list_{nullptr};
    std::unique_ptr<ProcessSampler> sampler_;
};

// Connection state of a machine (the supervisor-gui's view of it).
// The actual transport health is tracked by GrpcClient::is_connected();
// MachinesPanel synthesizes the user-facing label.
enum class MachineConnState {
    Disconnected,   // explicitly disconnected by the operator
    Connecting,     // a GrpcClient is up but no frames yet
    Connected,      // at least one frame received recently
    Down,           // was Connected, then frames stopped arriving
};

// One row in the machines side panel — keyed by name, with a status
// glyph + the address+port so the operator can sanity-check the
// rig config.
struct MachineRow {
    std::string       name;
    std::string       address;   // host:port for display
    MachineConnState  state{MachineConnState::Disconnected};
};

// "Machines" — left-side side panel (sibling to the notebook).
// Lists every target machine from the loaded machines.yaml, with a
// colored dot for connection state. Right-click for Connect /
// Disconnect / Show in Trace. Selecting a row broadcasts the
// machine name on EVT_MACHINE_FOCUS so other panels can scope.
class MachinesPanel : public wxPanel {
public:
    explicit MachinesPanel(wxWindow* parent);
    ~MachinesPanel() override;

    // Replace the row set wholesale (initial load + reload from the
    // File → Refresh Machines menu).
    void set_machines(std::vector<MachineRow> rows);

    // Update one row's state. Called by MainFrame's status timer.
    void set_state(const std::string& machine_name, MachineConnState s);

    // Currently-focused (selected) machine, or empty when no row
    // is selected.
    std::string focused() const;

private:
    void on_right_click(wxMouseEvent& evt);
    void on_select(wxListEvent& evt);
    void redraw();

    ::wxListCtrl*           list_{nullptr};
    std::vector<MachineRow> rows_;
};

// Posted whenever the selected row changes. Carries the machine
// name in GetString() — empty means "selection cleared".
wxDECLARE_EVENT(EVT_MACHINE_FOCUS, wxCommandEvent);

// "Table Viewer (etcd)" — observer's ETS-tab analogue, but the
// backing store is etcd. Browse any key under a user-typed prefix;
// optionally Watch the prefix for live updates. Operator tool only:
// applications go through services/db (typed Put/Get with schema
// versioning), never poke etcd directly. The GUI's role here is
// the supervisor-gui equivalent of `etcdctl` with a mouse.
//
// Does NOT inherit PanelBase — etcd is an independent data source,
// not driven by the supervisor's gRPC frames. Owns its etcd client
// + watcher; the rest of the GUI ignores it.
class EtcdPanelImpl;  // pimpl — keeps etcd-cpp-apiv3 headers out of panels.h
class EtcdPanel : public wxPanel {
public:
    explicit EtcdPanel(wxWindow* parent);
    ~EtcdPanel() override;

private:
    std::unique_ptr<EtcdPanelImpl> impl_;
};

// "Trace" — Wireshark-style: virtual list of live trace records on top
// (TIME | MACHINE | KIND | SRC | DST | MSG TYPE), lazy tree-decode of the
// selected row below (Header / Subject / Raw payload). Driven by the
// TraceStream egress (tag 0x0005) from com's TraceForwarder (:7710) — the
// SAME message traces `tdb logcat` / `rtdb logcat` show.
class TracePanelImpl;  // defined in trace_panel.cpp
class TracePanel : public PanelBase {
public:
    explicit TracePanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;

private:
    // Held for back-compat with code that references the old single-
    // list shape; points at the new virtual list inside impl_.
    ::wxListCtrl* list_{nullptr};
    TracePanelImpl* impl_{nullptr};
};

}  // namespace sup_gui
