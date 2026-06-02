// User handler bodies for SupervisorCtl.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/SupervisorCtl.hh.
//
// SupervisorCtl is the control front-end: it receives the nanopb control
// surface on TipcMux (bound in main.cc) and DIRECTLY drives the shared
// supervision engine (impl/core/runtime.*) — each handler calls eng->ctl_*()
// straight, no loop-marshal. The engine serializes those calls against its own
// loop tick with state_mu_ (recursive), so a CALL gets its reply synchronously
// on the TipcMux thread. SupervisorCtl also installs the bridge's EmitForwarder
// in init() — the engine's EmitSink fans events back out through this node's
// `events` broadcast senders, and the engine's restart re-push asks control (by
// child NAME) to resolve + cast the typed trace/log config.

#include "lib/SupervisorCtl.hh"

#include "core/bridge.h"
#include "core/runtime.h"

#include "NodeRef.hh"          // theia::runtime::cast(self, msg, TipcAddr)
#include "platform_runtime/runtime.pb.h"  // TraceControlPush / LogLevelPush

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ara::exec {

namespace {

// DIRECT control. SupervisorCtl's handlers call the engine's ctl_* methods
// straight (eng->ctl_*()), no run_on_engine loop-marshal / lambda / future. The
// engine serializes these against its loop tick with state_mu_ (see
// impl/core/runtime.*). `engine()` is the published pointer or nullptr before
// the worker constructs it.
::supervisor::Supervisor* engine() {
    return ::supervisor::supervisor_instance();
}

// Copy a fixed nanopb char[] field into a std::string (already NUL-terminated
// by nanopb's decode of a max_size:N field).
std::string s(const char* fixed) { return std::string(fixed); }

// Fill a ControlReply's fixed char[] fields safely.
void set_reply(ControlReply& rep, uint32_t status, const std::string& child,
               const std::string& message = "") {
    rep = ControlReply{};
    rep.status = status;
    std::snprintf(rep.child_name, sizeof(rep.child_name), "%s", child.c_str());
    std::snprintf(rep.message, sizeof(rep.message), "%s", message.c_str());
}

// ---- the EmitForwarder: engine event -> this node's broadcast senders -----
//
// The engine runs on the worker thread; these forwarders are called there and
// fan out via the lock-guarded broadcast_events_* (safe cross-thread). They
// need a SupervisorCtl* — published process-globally in init(). One pointer is
// enough (a single control node per process).
SupervisorCtl* g_ctl = nullptr;

void fwd_event(const ::supervisor::EventData& e) {
    if (!g_ctl) return;
    SupervisionEvent m{};
    m.kind         = e.kind;
    m.timestamp_ms = e.timestamp_ms;
    std::snprintf(m.child_name, sizeof(m.child_name), "%s", e.child_name.c_str());
    std::snprintf(m.supervisor_name, sizeof(m.supervisor_name), "%s",
                  e.supervisor_name.c_str());
    m.pid       = e.pid;
    m.exit_code = e.exit_code;
    std::snprintf(m.strategy, sizeof(m.strategy), "%s", e.strategy.c_str());
    std::snprintf(m.tombstone_path, sizeof(m.tombstone_path), "%s",
                  e.tombstone_path.c_str());
    std::snprintf(m.detail, sizeof(m.detail), "%s", e.detail.c_str());
    g_ctl->broadcast_events_event(m);
}

void fwd_health(const ::supervisor::HealthData& h) {
    if (!g_ctl) return;
    HealthBeacon m{};
    m.timestamp_ms     = h.timestamp_ms;
    m.uptime_ms        = h.uptime_ms;
    m.generation       = h.generation;
    m.total_workers    = h.total_workers;
    m.active_workers   = h.active_workers;
    m.total_restarts   = h.total_restarts;
    m.total_tombstones = h.total_tombstones;
    g_ctl->broadcast_events_health(m);
}

void fwd_snapshot_begin(uint64_t gen, uint64_t ts) {
    if (!g_ctl) return;
    SnapshotBegin m{};
    m.generation   = gen;
    m.timestamp_ms = ts;
    g_ctl->broadcast_events_snap_begin(m);
}

void fwd_edge(const ::supervisor::EdgeData& e) {
    if (!g_ctl) return;
    NodeEdge m{};
    m.op   = e.op;
    std::snprintf(m.parent_name, sizeof(m.parent_name), "%s", e.parent_name.c_str());
    std::snprintf(m.name, sizeof(m.name), "%s", e.name.c_str());
    m.kind = e.kind;
    g_ctl->broadcast_events_edge(m);
}

void fwd_node_state(const ::supervisor::NodeStateData& n) {
    if (!g_ctl) return;
    NodeState m{};
    std::snprintf(m.name, sizeof(m.name), "%s", n.name.c_str());
    m.pid            = n.pid;
    m.tid            = n.tid;
    m.state          = n.state;
    m.flags          = n.flags;
    m.restart_count  = n.restart_count;
    m.last_exit_code = n.last_exit_code;
    m.uptime_ms      = n.uptime_ms;
    m.cpu_pct        = n.cpu_pct;
    m.rss_kb         = n.rss_kb;
    m.vsz_kb         = n.vsz_kb;
    m.threads        = n.threads;
    m.shared_kb      = n.shared_kb;
    m.data_kb        = n.data_kb;
    g_ctl->broadcast_events_node_state(m);
}

void fwd_snapshot_end(uint64_t gen) {
    if (!g_ctl) return;
    SnapshotEnd m{};
    m.generation = gen;
    g_ctl->broadcast_events_snap_end(m);
}

// ---- LIFTED: resolve a child + cast a typed control message to it ----------
//
// The ONE place "resolve node, then cast" lives. Called BOTH by the handlers
// (live ConfigureTrace/ConfigureLogLevel) AND by the engine's restart re-push
// (via the EmitSink set_trace/set_log_level callbacks). The engine asks BY
// NAME; we resolve the child's address (engine read) and cast the runtime
// message from g_ctl (the runtime-backed node) — the worker runnable never
// touches transport. Same wire the child's register_cast<Msg> decodes.
template <typename Msg>
void resolve_and_cast(const std::string& child, const Msg& m) {
    if (!g_ctl) return;
    auto* eng = ::supervisor::supervisor_instance();
    if (!eng) return;
    const auto addr = eng->resolve_node(child);
    if (!addr.ok) return;
    ::theia::runtime::cast(*g_ctl, m,
                           ::theia::runtime::TipcAddr{addr.type, addr.instance});
}

// Typed entry points — the handlers pass the message straight off the wire.
void ctl_set_trace(const std::string& child,
                   const platform_runtime_TraceControlPush& m) {
    resolve_and_cast(child, m);
}
void ctl_set_log_level(const std::string& child,
                       const platform_runtime_LogLevelPush& m) {
    resolve_and_cast(child, m);
}

// Ordinal entry points — the engine (protobuf-free) passes plain values; we
// build the typed message here. Used by the restart re-push.
void ctl_set_trace(const std::string& child, uint32_t kind, bool enabled) {
    platform_runtime_TraceControlPush m{};
    m.kind    = static_cast<platform_runtime_TraceKind>(kind);
    m.enabled = enabled;
    resolve_and_cast(child, m);
}
void ctl_set_log_level(const std::string& child, uint32_t level) {
    platform_runtime_LogLevelPush m{};
    m.level = static_cast<platform_runtime_LogLevelValue>(level);
    resolve_and_cast(child, m);
}

}  // namespace


// ---- OTP init/1 — publish this node + install the EmitForwarder so the
//      engine's events fan out through our `events` broadcast senders.
void SupervisorCtl::init(SupervisorCtlState& /*s*/) {
    g_ctl = this;
    ::supervisor::EmitForwarder fwd;
    fwd.on_event          = &fwd_event;
    fwd.on_health         = &fwd_health;
    fwd.on_snapshot_begin = &fwd_snapshot_begin;
    fwd.on_edge           = &fwd_edge;
    fwd.on_node_state     = &fwd_node_state;
    fwd.on_snapshot_end   = &fwd_snapshot_end;
    fwd.set_trace         = [](const char* c, uint32_t k, bool e) {
        ctl_set_trace(c, k, e);
    };
    fwd.set_log_level     = [](const char* c, uint32_t l) {
        ctl_set_log_level(c, l);
    };
    ::supervisor::set_emit_forwarder(fwd);
}

// ---- string handle_info — the post_info()/send_after() tick path.
void SupervisorCtl::handle_info(const char* /*info*/, SupervisorCtlState& /*s*/) {
}


void SupervisorCtl::handle_cast(const HeartbeatReport& msg,
                                 SupervisorCtlState& /*s*/) {
    if (auto* e = engine())
        e->ctl_on_heartbeat(s(msg.node_name), static_cast<pid_t>(msg.pid),
                            msg.seq);
}

void SupervisorCtl::handle_cast(const SendTimeoutReport& msg,
                                 SupervisorCtlState& /*s*/) {
    if (auto* e = engine())
        e->ctl_on_send_timeout(s(msg.caller_node), s(msg.callee_node),
                               s(msg.iface), s(msg.method),
                               msg.budget_ms, msg.observed_ms);
}



TreeSnapshot SupervisorCtl::handle_call(
        const GetTreeRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    // Point-in-time snapshot for `tdb ps` / `tdb supervisor`: walk the engine's
    // tree into the flat parent-keyed children[] (the caller rebuilds the
    // hierarchy by name, same shape the NodeEdge/NodeState firehose streams).
    auto* eng = engine();
    auto rows = eng ? eng->ctl_get_tree() : std::vector<::supervisor::TreeRow>{};
    TreeSnapshot snap{};
    const pb_size_t cap =
        static_cast<pb_size_t>(sizeof(snap.children) / sizeof(snap.children[0]));
    for (const auto& r : rows) {
        if (snap.children_count >= cap) break;  // truncate (logged on encode)
        auto& c = snap.children[snap.children_count++];
        c = ChildState{};
        std::snprintf(c.name, sizeof(c.name), "%s", r.name.c_str());
        std::snprintf(c.parent_name, sizeof(c.parent_name), "%s",
                      r.parent_name.c_str());
        c.kind           = r.kind;
        c.pid            = r.pid;
        c.state          = r.state;
        c.restart_count  = r.restart_count;
        c.last_exit_code = r.last_exit_code;
        c.flags          = r.flags;
        std::snprintf(c.strategy, sizeof(c.strategy), "%s", r.strategy.c_str());
        std::snprintf(c.start_cmd, sizeof(c.start_cmd), "%s",
                      r.start_cmd.c_str());
    }
    return snap;
}

ChildState SupervisorCtl::handle_call(
        const GetChildRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    // Single-child query. The live per-node state rides the NodeState firehose;
    // a direct read-back isn't wired post-com-retirement. Return an empty row.
    return ChildState{};
}

ControlReply SupervisorCtl::handle_call(
        const StartChildRequest& req,
        SupervisorCtlState& /*s*/) {
    const auto& spec = req.spec;
    const std::string name   = s(spec.name);
    const std::string parent = s(spec.parent_supervisor);
    std::vector<std::string> cmd;
    for (pb_size_t i = 0; i < spec.start_cmd_count; ++i) cmd.push_back(s(spec.start_cmd[i]));
    std::vector<std::string> mods;
    for (pb_size_t i = 0; i < spec.modules_count; ++i) mods.push_back(s(spec.modules[i]));
    const int restart = static_cast<int>(spec.restart);
    const int shutdown = spec.shutdown;
    const int type = static_cast<int>(spec.type);

    auto* eng = engine();
    uint32_t status = eng
        ? eng->ctl_start_child(parent, name, cmd, restart, shutdown, type, mods)
        : 4 /*invalid_request when no engine*/;
    ControlReply rep;
    set_reply(rep, status, name);
    return rep;
}

ControlReply SupervisorCtl::handle_call(
        const DeleteChildRequest& req,
        SupervisorCtlState& /*s*/) {
    const std::string name = s(req.name);
    auto* eng = engine();
    uint32_t status = eng ? eng->ctl_delete_child(name) : 4;
    ControlReply rep;
    set_reply(rep, status, name);
    return rep;
}

// RestartChild AND TerminateChild both take ChildSelector, so the runtime
// dispatches both to THIS single overload (dedup by request type). We branch on
// the selector's `no_restart` discriminator: no_restart=true is the OTP-faithful
// terminate-and-HOLD (stop the child, skip restart policy + watchdog — RF mocks
// the node's TIPC addr from a probe; a later StartChild clears the hold);
// no_restart=false is RestartChild (stop then start).
ControlReply SupervisorCtl::handle_call(
        const ChildSelector& req,
        SupervisorCtlState& /*s*/) {
    const std::string name = s(req.name);
    const bool hold = req.no_restart;
    auto* eng = engine();
    uint32_t status = !eng ? 4
                     : hold ? eng->ctl_suspend_child(name)   // terminate + hold
                            : eng->ctl_restart_child(name);   // stop then start
    ControlReply rep;
    set_reply(rep, status, name);
    return rep;
}

ControlReply SupervisorCtl::handle_call(
        const Stop& /*req*/,
        SupervisorCtlState& /*s*/) {
    if (auto* eng = engine()) eng->request_shutdown();
    ControlReply rep;
    set_reply(rep, 0, "", "shutting down");
    return rep;
}

SystemInfo SupervisorCtl::handle_call(
        const GetSystemInfoRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    auto* eng = engine();
    ::supervisor::SystemInfoData info =
        eng ? eng->ctl_get_system_info() : ::supervisor::SystemInfoData{};
    SystemInfo m{};
    std::snprintf(m.hostname, sizeof(m.hostname), "%s", info.hostname.c_str());
    std::snprintf(m.kernel, sizeof(m.kernel), "%s", info.kernel.c_str());
    std::snprintf(m.os_pretty_name, sizeof(m.os_pretty_name), "%s",
                  info.os_pretty_name.c_str());
    m.cpu_count    = info.cpu_count;
    m.total_ram_kb = info.total_ram_kb;
    m.uptime_sec   = info.uptime_sec;
    std::snprintf(m.theia_git_sha, sizeof(m.theia_git_sha), "%s",
                  info.theia_git_sha.c_str());
    std::snprintf(m.build_timestamp, sizeof(m.build_timestamp), "%s",
                  info.build_timestamp.c_str());
    m.start_timestamp_ms = info.start_timestamp_ms;
    return m;
}

ControlReply SupervisorCtl::handle_call(
        const ConfigureTraceRequest& req,
        SupervisorCtlState& /*s*/) {
    const auto& cfg = req.config;
    const std::string target = s(cfg.target_node);
    ControlReply rep;
    auto* eng = ::supervisor::supervisor_instance();
    if (!eng) { set_reply(rep, 4, target, "engine not up"); return rep; }

    // Store (survives restart) — returns false if the target doesn't resolve.
    if (!eng->ctl_configure_trace(target, cfg.trace_ctrl.enabled,
                                  static_cast<uint32_t>(cfg.trace_ctrl.kind))) {
        set_reply(rep, 4 /*invalid_request*/, target,
                  "no worker or node by that name");
        return rep;
    }
    // Push live — the lifted resolve+cast, cfg.trace_ctrl verbatim. The SAME
    // function the engine's restart re-push calls (by ordinal).
    ctl_set_trace(target, cfg.trace_ctrl);
    set_reply(rep, 0, target, "trace config applied");
    return rep;
}

TraceConfigList SupervisorCtl::handle_call(
        const GetTraceConfigRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    auto* eng = engine();
    auto rows = eng ? eng->ctl_get_trace_config()
                    : std::vector<::supervisor::TraceConfigRow>{};
    TraceConfigList list{};
    list.configs_count = 0;
    for (const auto& r : rows) {
        if (list.configs_count >=
            static_cast<pb_size_t>(sizeof(list.configs) / sizeof(list.configs[0]))) {
            break;  // fixed array full; truncate (logged by nanopb on encode)
        }
        auto& c = list.configs[list.configs_count++];
        c = system_supervisor_TraceConfig{};
        std::snprintf(c.target_node, sizeof(c.target_node), "%s",
                      r.target_node.c_str());
        // TraceConfig embeds the runtime TraceControlPush now; fill it from the
        // stored row. (msg_type is no longer carried.)
        c.has_trace_ctrl   = true;
        c.trace_ctrl.kind  = static_cast<platform_runtime_TraceKind>(r.kind);
        c.trace_ctrl.enabled = true;
    }
    return list;
}

ControlReply SupervisorCtl::handle_call(
        const ConfigureLogLevelRequest& req,
        SupervisorCtlState& /*s*/) {
    const auto& cfg = req.config;
    const std::string target = s(cfg.target_node);
    ControlReply rep;
    auto* eng = ::supervisor::supervisor_instance();
    if (!eng) { set_reply(rep, 4, target, "engine not up"); return rep; }

    // Store (survives restart) — returns false if the target doesn't resolve.
    if (!eng->ctl_configure_log_level(
            target, static_cast<uint32_t>(cfg.log_level.level))) {
        set_reply(rep, 4, target, "no worker or node by that name");
        return rep;
    }
    // Push live — the lifted resolve+cast, cfg.log_level verbatim.
    ctl_set_log_level(target, cfg.log_level);
    set_reply(rep, 0, target, "log level applied");
    return rep;
}


}  // namespace ara::exec
