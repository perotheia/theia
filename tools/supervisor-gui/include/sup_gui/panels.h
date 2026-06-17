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
class wxTreeListEvent;
class wxDataViewEvent;
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

    // The GUI scopes the data panels to ONE machine (the left-dock selection).
    // When the focus changes, MainFrame calls this so a panel that accumulates
    // per-machine state (a box / rows per machine) can drop everything except
    // the now-focused machine. Default: no-op (single-machine panels ignore it).
    virtual void set_machine_filter(const std::string& /*machine_name*/) {}
};

// "System" — host + build + supervisor facts, one wxStaticBox per machine.
// Observer-style "System" tab: per machine, four sub-boxes in a 2×2 grid
// (the Erlang observer System tab, Theia-native — no Erlang-VM Memory/limits
// boxes). Data comes from the same surface `tdb info` / `rtdb info` show:
//   - "System & Architecture" — SystemInfo (tag 0x0004): hostname, kernel, os,
//     cpus, theia git sha, build ts.
//   - "Resources" — SystemInfo: ram (MB/GB), disk / (used/total), disk install,
//     host uptime, supervisor started (local).
//   - "Supervisor Statistics" — HealthBeacon (tag 0x0002): generation, workers,
//     restarts, tombstones, last heartbeat. (Empty until the supervisor emits
//     health frames; SystemInfo is the live content today.)
//   - "GPU / Accelerators" — placeholder ("awaiting shwa feed") until the shwa
//     FC's nvidia-smi/jtop telemetry path is wired (a follow-up).
class SystemPanel : public PanelBase {
public:
    explicit SystemPanel(wxWindow* parent);
    void on_frame(const std::string& machine_name, uint16_t tag,
                  const std::string& payload) override;
    void set_machine_filter(const std::string& machine_name) override;

private:
    // Per-box row counts. Pre-allocated key/value wxStaticTexts updated in
    // place to avoid relayout churn.
    static constexpr int kArchRows   = 6;  // hostname,kernel,os,cpus,sha,build
    static constexpr int kResRows    = 5;  // ram,disk /,disk install,uptime,started
    static constexpr int kHealthRows = 5;  // gen,workers,restarts,tombstones,beat
    // GPU / Accelerators — SHWA AccelSample (tag 0x0006): board, gpu util,
    // gpu freq, temp, power, fan.
    static constexpr int kGpuRows    = 6;  // board,gpu%,gpu freq,temp,power,fan
    struct MachineRows {
        wxStaticBox*       box{nullptr};   // outer per-machine box
        wxStaticBoxSizer*  sizer{nullptr};
        wxStaticText*      arch_vals  [kArchRows]{};
        wxStaticText*      res_vals   [kResRows]{};
        wxStaticText*      health_vals[kHealthRows]{};
        wxStaticText*      gpu_vals   [kGpuRows]{};
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

    // #365 — wire a callback the panel calls when the user clicks Apply in the
    // right-click ConfigureTrace dialog. main_frame looks up the matching
    // GrpcClient and calls configure_trace(node, msg, enabled, kind). `kind` is
    // a TraceKind ordinal (0=all, 1=CAST_OUT … 5=STATEM) — the dimension the
    // supervisor's per-node trace filter keys on (#403). Lifetime: the panel
    // captures by value, so the closure may safely outlive the call.
    using ConfigureTraceCallback = std::function<void(
        const std::string& /*machine*/, const std::string& /*node*/,
        const std::string& /*msg_type*/, bool /*enabled*/, uint32_t /*kind*/)>;
    void set_configure_trace_callback(ConfigureTraceCallback cb);

    // GUI-3 context menu. Each is invoked from the right-click menu on the
    // selectable tree; main_frame routes to the matching machine's GrpcClient.
    //
    // Log level — menu item on EITHER an FC (worker) or a node. The supervisor
    // ConfigureLogLevel keys on the target name (worker name for an FC; the
    // node's kNodeName for a granular per-node level). `level` is a name
    // (TRACE/DEBUG/INFO/WARN/ERROR). Returns a short status string for the bar.
    using LogLevelCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*target*/,
        const std::string& /*level*/)>;
    void set_log_level_callback(LogLevelCallback cb);

    // Restart a subtree (right-click on a supervisor) OR kill an FC (worker):
    // both map to RestartChild on the named child — the supervisor restarts the
    // worker, or every worker under the named supervisor. Returns a status line.
    using RestartCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*name*/)>;
    void set_restart_callback(RestartCallback cb);

    // Download a crashed FC's tombstone — gated on the CORE_DUMPED flag. The
    // callback fetches the (capped) bytes via GrpcClient::get_tombstone and is
    // responsible for surfacing them (save dialog / viewer). Returns a status
    // line for the bar.
    using TombstoneCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*fc_name*/)>;
    void set_tombstone_callback(TombstoneCallback cb);

    // Kill a PROCESS (right-click on a proc row). Maps to RestartChild on the
    // process name (kill + supervisor restarts it). Returns a status line.
    using KillCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*proc*/)>;
    void set_kill_callback(KillCallback cb);

    // Fetch a node's CURRENT effective log level (for the Log level submenu's
    // checkmark). Returns the level name (TRACE/DEBUG/INFO/WARN/ERROR) or ""
    // if unknown. main_frame wires it to GrpcClient::get_log_level for `node`.
    using GetLogLevelCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*node*/)>;
    void set_get_log_level_callback(GetLogLevelCallback cb);

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
// One TIPC socket owned by a process (from ChildState.sockets). rx/tx are the
// kernel's current receive/transmit queue depth in bytes (per-socket backlog).
struct SocketRow {
    uint64_t    inode    = 0;
    uint32_t    state    = 0;
    uint32_t    rx_queue = 0;
    uint32_t    tx_queue = 0;
    std::string local;
    std::string remote;
};

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
    // TIPC: per-process totals (summed over the node's sockets) + the detail.
    uint32_t    tipc_rx         = 0;   // summed receive-queue bytes
    uint32_t    tipc_tx         = 0;   // summed transmit-queue bytes
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
    void set_machine_filter(const std::string& machine_name) override;

    // Right-click → Kill / Remove. main_frame wires this to the matching
    // machine's GrpcClient. `op` is "kill" (RestartChild) or "remove"
    // (TerminateChild, no_restart=true). Returns a human status line.
    using ChildOpCallback = std::function<std::string(
        const std::string& /*machine*/, const std::string& /*name*/,
        const std::string& /*op: kill|remove*/)>;
    void set_child_op_callback(ChildOpCallback cb);

private:
    void on_sampler_update(wxThreadEvent& evt);   // legacy, no-op now
    void refresh_list();
    void on_col_click(wxDataViewEvent& evt);      // header click → set sort
    void on_context_menu(wxTreeListEvent& evt);   // right-click → kill/remove
    ChildOpCallback child_op_cb_;

    // Latest rows, indexed by machine_name. Each snapshot replaces
    // that machine's vector; rows from other machines stay intact.
    std::map<std::string, std::vector<ProcessRow>> rows_by_machine_;

    ::wxTreeListCtrl*               list_{nullptr};
    std::unique_ptr<ProcessSampler> sampler_;

    // Sort state — which column to sort by + direction. Set by clicking a
    // column header (toggles asc/desc on the same column). -1 = default
    // (machine, name). Applied in refresh_list so it survives the 1Hz refresh.
    int  sort_col_{-1};
    bool sort_desc_{false};
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

// One row in a side-panel list — keyed by name, with a status glyph + an
// address/where string. Used for BOTH the Connections list (a saved com
// endpoint, host:port) and the Machines list (a machine discovered through the
// connected endpoint).
struct MachineRow {
    std::string       name;
    std::string       address;   // host:port (connections) / "(via …)" (machines)
    MachineConnState  state{MachineConnState::Disconnected};
};

// "Connections + Machines" — the left-side dock, TWO sections in one panel:
//
//   Connections   — saved com endpoints (127.0.0.1, jetson, rpi4, …), each with
//                   a connect-state glyph. ONLY ONE is connected at a time;
//                   selecting one (or right-click → Connect) switches the hub.
//                   Persisted to ~/.config/theia-gui/connections.json.
//   Machines      — the machines the connected endpoint's com aggregates (the
//                   local one + each mN/ peer). Selecting one scopes EVERY data
//                   panel (Applications/Processes/System/Load) to that single
//                   machine — N machines per connection can't all share the UX.
//
// Connection selection posts EVT_CONNECTION_SELECT (the host:port in GetString);
// machine selection posts EVT_MACHINE_FOCUS (the machine name). Right-click on a
// connection → Connect/Disconnect; on a machine → Show in Trace.
class MachinesPanel : public wxPanel {
public:
    explicit MachinesPanel(wxWindow* parent);
    ~MachinesPanel() override;

    // ---- Connections (top section) ---------------------------------------
    // Replace the saved-connection rows wholesale (initial load from file).
    void set_connections(std::vector<MachineRow> rows);
    // Add a connection if not already listed (Connect… dialog). Idempotent.
    void add_connection(const std::string& name, const std::string& host_port,
                        MachineConnState s);
    // Update one connection's state (status timer). Only the active one is
    // Connected; the rest are Disconnected.
    void set_connection_state(const std::string& host_port, MachineConnState s);
    // The selected connection's host:port (or first, or empty).
    std::string selected_connection() const;
    // All saved connections (for persistence).
    std::vector<MachineRow> connections() const { return conns_; }

    // ---- Machines (bottom section) ---------------------------------------
    // Replace the machine rows (called on (re)connect to reset the list).
    void set_machines(std::vector<MachineRow> rows);
    // Add a discovered machine if not already listed (idempotent).
    void add_machine(const std::string& name, const std::string& address,
                     MachineConnState s);
    void set_state(const std::string& machine_name, MachineConnState s);
    // The selected machine name (panels scope to this), or first/empty.
    std::string focused() const;
    // Programmatically select a machine row by name (auto-select-first).
    void select_machine(const std::string& name);

private:
    void on_conn_right_click(wxMouseEvent& evt);
    void on_machine_right_click(wxMouseEvent& evt);
    void on_conn_select(wxListEvent& evt);
    void on_machine_select(wxListEvent& evt);
    void redraw_conns();
    void redraw_machines();

    ::wxListCtrl*           conn_list_{nullptr};
    ::wxListCtrl*           mach_list_{nullptr};
    std::vector<MachineRow> conns_;
    std::vector<MachineRow> rows_;     // machines
};

// Posted whenever the selected MACHINE row changes. Carries the machine
// name in GetString() — empty means "selection cleared".
wxDECLARE_EVENT(EVT_MACHINE_FOCUS, wxCommandEvent);

// Posted whenever the selected CONNECTION row changes (or a connection
// context-menu action fires). GetString() = the connection's host:port; GetInt()
// == 0 for a plain select, or an ID_CTX_* menu id for a context action.
wxDECLARE_EVENT(EVT_CONNECTION_SELECT, wxCommandEvent);

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

// "Persistency" — services/per's TYPED view, proxied through com's PerView
// gRPC (distinct from the raw etcd Table Viewer): the schema registry
// (ListSchemas) + a Snapshot button (trigger a config backup). Does NOT
// inherit PanelBase — it's request/response (not driven by the Subscribe
// frame stream). main_frame wires its two callbacks to the focused machine's
// GrpcClient (list_schemas / snapshot), like the ConfigureTrace callback.
class PersistencyPanelImpl;
class PersistencyPanel : public wxPanel {
public:
    explicit PersistencyPanel(wxWindow* parent);
    ~PersistencyPanel() override;

    // (config_type="" → all) → rows + ok flag. Set by main_frame.
    struct Schema { std::string config_type; std::string digest; };
    using ListSchemasCallback =
        std::function<std::vector<Schema>(const std::string& config_type,
                                          bool* ok)>;
    // (label) → (status, message). Set by main_frame.
    using SnapshotCallback =
        std::function<int(const std::string& label, std::string* msg)>;
    void set_callbacks(ListSchemasCallback ls, SnapshotCallback snap);

private:
    std::unique_ptr<PersistencyPanelImpl> impl_;
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
