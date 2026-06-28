// User handler bodies for SupervisorCtl.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/SupervisorCtl.hh.
//
// SupervisorCtl is the control front-end: it receives the nanopb control
// surface on TipcMux (bound in main.cc) and drives the shared supervision
// engine (impl/core/runtime.*) through the ACTOR command queue — each handler
// builds a typed ExecCommand and either enqueue()s it (cast: heartbeat,
// send-timeout, shutdown) or call()s it (CALL: start/restart/delete/configure
// status, GetTree/SystemInfo/TraceConfig reads). The engine's loop thread is
// the sole writer of the tree; no state lock is exposed. SupervisorCtl also
// installs the bridge's EmitForwarder in init() — the engine's EmitSink fans
// events back out through this node's `events` broadcast senders, and the
// engine's restart re-push asks control (by child NAME) to resolve + cast the
// typed trace/log config (resolve off the lock-free Registry, cast from here).

#include "lib/SupervisorCtl.hh"

#include "core/bridge.h"
#include "core/runtime.h"

#include "NodeRef.hh"          // theia::runtime::cast(self, msg, TipcAddr)
#include "TheiaMsgHeader.hh"   // PgMembership RDM-push frame header
#include "platform_runtime/runtime.pb.h"  // TraceControlPush / LogLevelPush

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <sys/socket.h>       // PgMembership RDM-datagram push to a watcher
#include <linux/tipc.h>
#include <unistd.h>
#include <pb_encode.h>

namespace ara::exec {

namespace {

// The engine handle (process-global, published by SupervisorWorker once it
// constructs the engine). nullptr before then — every caller null-checks.
// Handlers hand typed ExecCommands to engine()->enqueue()/call(); they never
// touch engine state directly.
::supervisor::Supervisor* engine() {
    return ::supervisor::supervisor_instance();
}

// A default ExecCommand with just the op set — avoids brace-init's
// -Wmissing-field-initializers on the many trailing arg fields.
::supervisor::ExecCommand exec_cmd(::supervisor::ExecCommand::Op op) {
    ::supervisor::ExecCommand c;
    c.op = op;
    return c;
}

// The supervisor as a trace/log TARGET means ITSELF, not a child. `tdb trace
// root` (the supervision tree's root name, shown by `tdb ps`) or `tdb trace sup`
// redirects to the supervisor's OWN runtime: only the gen_server node
// (supervisor_ctl) has a Tracer — the worker runnable is a bare thread. So a
// self-target flips tracer_for("supervisor_ctl") IN-PROCESS, with no Registry
// resolve (which would reject "root") and no TIPC cast (there is no child node).
bool is_self_target(const std::string& target) {
    return target == "root" || target == "sup";
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
// multicast via pg_emit() (PG name-sequence, local type resolve — see below).
// They need a SupervisorCtl* — published process-globally in init(). One pointer
// is enough (a single control node per process).
SupervisorCtl* g_ctl = nullptr;

// The supervisor IS the PG allocator, so it must NOT use the generated
// broadcast_*() (which TIPC-self-CALLs pg_resolve → would block the engine loop
// on a connect to its own control address). Instead it resolves each event
// group's TIPC type LOCALLY from the engine registry (pg_local_type, loop-thread
// only — these forwarders run on the engine/worker thread) and multicasts via
// pg_broadcast<T>(). group_name = the wire type name (msg_type_name<T>()), the
// same well-known .art name a member passes to pg_join. See [[runnable-control-and-heartbeat]].
template <typename T>
void pg_emit(const T& m) {
    if (!g_ctl) return;
    auto* eng = ::supervisor::supervisor_instance();
    if (!eng) return;
    // group_name = the wire type name — IDENTICAL to what a member's
    // pg_join<T>() passes (msg_type_name<T>()), so allocator key + member key
    // match by construction. No hardcoded strings.
    g_ctl->pg_broadcast<T>(
        eng->pg_local_type(::theia::runtime::msg_type_name<T>()), m);
}

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
    pg_emit(m);
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
    pg_emit(m);
}

void fwd_snapshot_begin(uint64_t gen, uint64_t ts) {
    if (!g_ctl) return;
    SnapshotBegin m{};
    m.generation   = gen;
    m.timestamp_ms = ts;
    pg_emit(m);
}

void fwd_edge(const ::supervisor::EdgeData& e) {
    if (!g_ctl) return;
    NodeEdge m{};
    m.op   = e.op;
    std::snprintf(m.parent_name, sizeof(m.parent_name), "%s", e.parent_name.c_str());
    std::snprintf(m.name, sizeof(m.name), "%s", e.name.c_str());
    m.kind = e.kind;
    pg_emit(m);
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
    pg_emit(m);
}

void fwd_snapshot_end(uint64_t gen) {
    if (!g_ctl) return;
    SnapshotEnd m{};
    m.generation = gen;
    pg_emit(m);
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
    // Resolve straight off the engine's immutable Registry (the manifest index)
    // — no command-queue hop: the name→address map is fixed at load and safe to
    // read from any thread. Then cast from g_ctl (this runtime-backed thread);
    // the worker runnable can't touch transport.
    const auto addr = eng->registry().resolve(child);
    if (!addr.ok) return;
    // Short connect budget: a trace/log push to a child that ISN'T listening
    // (down / not yet up) must NOT stall this control thread for the 3s connect
    // default — that wedges the whole SupervisorCtl surface. 250ms is plenty for
    // a live local peer; a dead one fails fast. The store already happened, and
    // the heartbeat-after-gap re-push re-applies once the child is back.
    ::theia::runtime::cast(*g_ctl, m,
                           ::theia::runtime::TipcAddr{addr.type, addr.instance},
                           /*dst_name=*/nullptr, /*connect_timeout_ms=*/250);
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

// OTP pg:monitor — cast a PgMembership to ONE watcher's EXPLICIT address (not a
// node name; the watcher gave its bound addr in PgWatch). Builds the nanopb
// PgMembership from the flat member array the engine handed us. Same 250ms
// fast-fail connect budget as resolve_and_cast.
void ctl_push_pg_membership(uint32_t watcher_type, uint32_t watcher_instance,
                            const char* group_name, uint32_t group_type,
                            const uint32_t* members_flat, uint32_t count) {
    PgMembership m{};
    m.status     = 0;
    std::snprintf(m.group_name, sizeof(m.group_name), "%s",
                  group_name ? group_name : "");
    m.group_type = group_type;
    m.members_count = count > 64 ? 64 : count;
    for (uint32_t i = 0; i < m.members_count; ++i) {
        m.members[i].tipc_type     = members_flat[2 * i];
        m.members[i].tipc_instance = members_flat[2 * i + 1];
    }
    // Encode + RDM-datagram sendto the watcher's bound recv socket. PG recv
    // sockets are SOCK_RDM (the watcher's PgClient binds RDM); a SEQPACKET cast
    // would be silently dropped (wrong type) — the same socket-type trap as the
    // trace egress. So we sendto here, not ::cast. service_id = PgMembership's id
    // (the watcher's PgClient dispatch_frame_ routes it by kMembershipSid).
    uint8_t pb[2048];
    pb_ostream_t os = pb_ostream_from_buffer(pb, sizeof(pb));
    if (!pb_encode(&os, system_supervisor_PgMembership_fields, &m)) return;
    int fd = ::socket(AF_TIPC, SOCK_RDM, 0);
    if (fd < 0) return;
    struct sockaddr_tipc a{};
    a.family                  = AF_TIPC;
    a.addrtype                = TIPC_ADDR_NAME;
    a.scope                   = TIPC_CLUSTER_SCOPE;
    a.addr.name.name.type     = watcher_type;
    a.addr.name.name.instance = watcher_instance;
    ::theia::runtime::TheiaMsgHeader hdr{};
    hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
    hdr.msg_type           = ::theia::runtime::kMsgGenCast;
    hdr.proto_len          = static_cast<uint16_t>(os.bytes_written);
    hdr.rpc.service_id     =
        ::theia::runtime::hash_msg_type_("system_supervisor_PgMembership");
    hdr.rpc.method_id      = 0;
    hdr.rpc.correlation_id = 0;
    std::string frame(sizeof(hdr) + os.bytes_written, '\0');
    std::memcpy(&frame[0], &hdr, sizeof(hdr));
    std::memcpy(&frame[sizeof(hdr)], pb, os.bytes_written);
    (void)::sendto(fd, frame.data(), frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
                   reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    ::close(fd);
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
    fwd.push_pg_membership = &ctl_push_pg_membership;   // OTP pg:monitor push
    ::supervisor::set_emit_forwarder(fwd);
}

// ---- string handle_info — the post_info()/send_after() tick path.
void SupervisorCtl::handle_info(const char* /*info*/, SupervisorCtlState& /*s*/) {
}


void SupervisorCtl::handle_cast(const HeartbeatReport& msg,
                                 SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* e = engine();
    if (!e) return;
    ::supervisor::ExecCommand c;
    c.op   = Op::OnHeartbeat;
    c.name = s(msg.node_name);
    c.pid  = static_cast<pid_t>(msg.pid);
    c.seq  = msg.seq;
    e->enqueue(std::move(c));
}

void SupervisorCtl::handle_cast(const SendTimeoutReport& msg,
                                 SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* e = engine();
    if (!e) return;
    ::supervisor::ExecCommand c;
    c.op          = Op::OnSendTimeout;
    c.name        = s(msg.caller_node);   // caller
    c.callee      = s(msg.callee_node);
    c.iface       = s(msg.iface);
    c.method      = s(msg.method);
    c.budget_ms   = msg.budget_ms;
    c.observed_ms = msg.observed_ms;
    e->enqueue(std::move(c));
}

// pg join/resolve (node → supervisor, CALL). The supervisor allocates the group's
// 0x8003 type (once per group_name = the wire message-type name) + a unique
// instance (join=true) or returns the type with instance 0 (resolve-only). The
// member binds {group_type, instance}; a broadcaster sends to {group_type, 0..~0}.
// pid is bookkeeping for the watchdog reap (frees the instance), NOT identity.
PgJoinReply SupervisorCtl::handle_call(const PgJoinReq& req,
                                       SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    PgJoinReply rep{};
    auto* eng = engine();
    if (!eng) { rep.status = 4; return rep; }   // invalid_request (no engine)
    ::supervisor::ExecCommand c;
    c.op         = Op::PgJoin;
    c.name       = s(req.node_name);
    c.pid        = static_cast<pid_t>(req.pid);   // member's pid → watchdog reap
    c.group_name = s(req.group_name);
    c.pg_join    = req.join;
    auto r = eng->call(std::move(c));
    rep.status     = r.status;
    rep.group_type = r.pg_group_type;
    rep.instance   = r.pg_instance;
    return rep;
}

ControlReply SupervisorCtl::handle_call(const PgLeaveReq& req,
                                        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    uint32_t status = 4;
    if (eng) {
        ::supervisor::ExecCommand c;
        c.op          = Op::PgLeave;
        c.name        = s(req.node_name);
        c.group_name  = s(req.group_name);
        c.group_type  = req.group_type;
        c.pg_instance = req.instance;
        status = eng->call(std::move(c)).status;
    }
    ControlReply rep;
    set_reply(rep, status, s(req.node_name));
    return rep;
}

// pg watch (OTP pg:monitor). A PRODUCER asks to monitor `group_name`: register
// its address as a watcher and return the current PgMembership; thereafter the
// engine casts a fresh PgMembership to that address on every join/leave/reap.
// watch=false demonitors. The reply IS the initial member list.
PgMembership SupervisorCtl::handle_call(const PgWatchReq& req,
                                        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    PgMembership rep{};
    auto* eng = engine();
    if (!eng) { rep.status = 4; return rep; }
    ::supervisor::ExecCommand c;
    c.op                 = Op::PgWatch;
    c.name               = s(req.node_name);
    c.pid                = static_cast<pid_t>(::getpid());  // watcher's process — for reap; overwritten below if probe
    c.group_name         = s(req.group_name);
    c.pg_member_type     = req.watcher_type;       // where to push updates
    c.pg_member_instance = req.watcher_instance;
    c.pg_watch           = req.watch;
    auto r = eng->call(std::move(c));
    rep.status     = r.status;
    std::snprintf(rep.group_name, sizeof(rep.group_name), "%s", s(req.group_name).c_str());
    rep.group_type = r.pg_group_type;
    rep.members_count = static_cast<pb_size_t>(
        r.pg_members.size() > 64 ? 64 : r.pg_members.size());
    for (pb_size_t i = 0; i < rep.members_count; ++i) {
        rep.members[i].tipc_type     = r.pg_members[i].first;
        rep.members[i].tipc_instance = r.pg_members[i].second;
    }
    return rep;
}



TreeSnapshot SupervisorCtl::handle_call(
        const GetTreeRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    // Point-in-time snapshot for `tdb ps` / `tdb supervisor`: walk the engine's
    // tree into the flat parent-keyed children[] (the caller rebuilds the
    // hierarchy by name, same shape the NodeEdge/NodeState firehose streams).
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    auto rows = eng ? eng->call(exec_cmd(Op::GetTree)).tree
                    : std::vector<::supervisor::TreeRow>{};
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
        // Resource metrics (workers only; 0 for supervisors/nodes) — carried in
        // the GetTree snapshot so `tdb ps` + the GUI Processes panel show live
        // cpu/mem/uptime, not zeros. (Per-thread threads_detail is max_count:0
        // in the GetTree wire — only the scalar aggregates ride here; the
        // per-thread breakdown stays on the NodeState firehose.)
        c.uptime_ms = r.uptime_ms;
        c.cpu_pct   = r.cpu_pct;
        c.rss_kb    = r.rss_kb;
        c.vsz_kb    = r.vsz_kb;
        c.shared_kb = r.shared_kb;
        c.data_kb   = r.data_kb;
        c.threads   = r.threads;
        // TIPC traffic summary (summed queue bytes over the node's sockets).
        // The full per-socket list stays off the wire (max_count:0) to keep the
        // TreeSnapshot under the TIPC reply cap.
        c.tipc_rx   = r.tipc_rx;
        c.tipc_tx   = r.tipc_tx;
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
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    uint32_t status = 4 /*invalid_request when no engine*/;
    if (eng) {
        ::supervisor::ExecCommand c;
        c.op        = Op::StartChild;
        c.parent    = parent;
        c.name      = name;
        c.start_cmd = std::move(cmd);
        c.modules   = std::move(mods);
        c.restart   = static_cast<int>(spec.restart);
        c.shutdown  = spec.shutdown;
        c.type      = static_cast<int>(spec.type);
        status = eng->call(std::move(c)).status;
    }
    ControlReply rep;
    set_reply(rep, status, name);
    return rep;
}

ControlReply SupervisorCtl::handle_call(
        const DeleteChildRequest& req,
        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    const std::string name = s(req.name);
    auto* eng = engine();
    uint32_t status = 4;
    if (eng) {
        auto c = exec_cmd(Op::DeleteChild);
        c.name = name;
        status = eng->call(std::move(c)).status;
    }
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
    using Op = ::supervisor::ExecCommand::Op;
    const std::string name = s(req.name);
    const bool hold = req.no_restart;
    auto* eng = engine();
    uint32_t status = 4;
    if (eng) {
        // hold=true → SuspendChild (terminate + hold); else RestartChild.
        auto c = exec_cmd(hold ? Op::SuspendChild : Op::RestartChild);
        c.name = name;
        status = eng->call(std::move(c)).status;
    }
    ControlReply rep;
    set_reply(rep, status, name);
    return rep;
}

ControlReply SupervisorCtl::handle_call(
        const Stop& /*req*/,
        SupervisorCtlState& /*s*/) {
    // Enqueue Shutdown (fire-and-forget): the loop sets shutdown_requested_ and
    // exits. We reply to the caller immediately — the wind-down is async.
    using Op = ::supervisor::ExecCommand::Op;
    if (auto* eng = engine()) eng->enqueue(exec_cmd(Op::Shutdown));
    ControlReply rep;
    set_reply(rep, 0, "", "shutting down");
    return rep;
}

SystemInfo SupervisorCtl::handle_call(
        const GetSystemInfoRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    ::supervisor::SystemInfoData info =
        eng ? eng->call(exec_cmd(Op::GetSystemInfo)).sysinfo
            : ::supervisor::SystemInfoData{};
    SystemInfo m{};
    std::snprintf(m.machine_name, sizeof(m.machine_name), "%s",
                  info.machine_name.c_str());
    std::snprintf(m.hostname, sizeof(m.hostname), "%s", info.hostname.c_str());
    std::snprintf(m.kernel, sizeof(m.kernel), "%s", info.kernel.c_str());
    std::snprintf(m.os_pretty_name, sizeof(m.os_pretty_name), "%s",
                  info.os_pretty_name.c_str());
    // Host hardware stats (cpu_count/total_ram/uptime/disk) moved to
    // services/shwa — SystemInfo carries supervisor identity only now.
    std::snprintf(m.theia_git_sha, sizeof(m.theia_git_sha), "%s",
                  info.theia_git_sha.c_str());
    std::snprintf(m.build_timestamp, sizeof(m.build_timestamp), "%s",
                  info.build_timestamp.c_str());
    std::snprintf(m.release_version, sizeof(m.release_version), "%s",
                  info.release_version.c_str());
    m.start_timestamp_ms = info.start_timestamp_ms;
    return m;
}

HealthBeacon SupervisorCtl::handle_call(
        const GetHealthRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    ::supervisor::HealthData h =
        eng ? eng->call(exec_cmd(Op::GetHealth)).health
            : ::supervisor::HealthData{};
    HealthBeacon m{};
    m.timestamp_ms     = h.timestamp_ms;
    m.uptime_ms        = h.uptime_ms;
    m.generation       = h.generation;
    m.total_workers    = h.total_workers;
    m.active_workers   = h.active_workers;
    m.total_restarts   = h.total_restarts;
    m.total_tombstones = h.total_tombstones;
    return m;
}

ControlReply SupervisorCtl::handle_call(
        const ConfigureTraceRequest& req,
        SupervisorCtlState& st) {
    using Op = ::supervisor::ExecCommand::Op;
    const auto& cfg = req.config;
    const std::string target = s(cfg.target_node);
    ControlReply rep;

    // Self-target: `tdb trace root` / `tdb trace sup` traces the supervisor
    // ITSELF, not a child. Flip THIS node's (supervisor_ctl) Tracer in-process
    // — no engine store, no Registry resolve, no TIPC cast. Only the gen_server
    // (this node) has a Tracer; the worker runnable doesn't. We reuse the
    // framework's TraceControlPush handler verbatim (the SAME logic a child runs
    // when the supervisor casts to it), so kind/enable semantics stay identical.
    if (is_self_target(target)) {
        this->handle_cast(cfg.trace_ctrl, st);  // base GenServer overload
        set_reply(rep, 0, target, "trace config applied (supervisor self)");
        return rep;
    }

    auto* eng = engine();
    if (!eng) { set_reply(rep, 4, target, "engine not up"); return rep; }

    // STORE on the loop thread (survives restart) — ok=false if the target
    // doesn't resolve. The store + resolve validation run where the tree lives.
    auto c = exec_cmd(Op::ConfigureTrace);
    c.name    = target;
    c.enabled = cfg.trace_ctrl.enabled;
    c.kind    = static_cast<uint32_t>(cfg.trace_ctrl.kind);
    if (!eng->call(std::move(c)).ok) {
        set_reply(rep, 4 /*invalid_request*/, target,
                  "no worker or node by that name");
        return rep;
    }
    // Push live FROM THIS (runtime-backed) thread — the lifted resolve+cast,
    // cfg.trace_ctrl verbatim. resolve runs on the loop (via call); the cast is
    // here (the worker runnable can't touch transport). SAME function the
    // engine's restart re-push calls (by ordinal).
    ctl_set_trace(target, cfg.trace_ctrl);
    set_reply(rep, 0, target, "trace config applied");
    return rep;
}

TraceConfigList SupervisorCtl::handle_call(
        const GetTraceConfigRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    auto rows = eng ? eng->call(exec_cmd(Op::GetTraceConfig)).trace_cfg
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

GetTombstoneReply SupervisorCtl::handle_call(
        const GetTombstoneRequest& req,
        SupervisorCtlState& /*s_*/) {
    using Op = ::supervisor::ExecCommand::Op;
    GetTombstoneReply rep{};
    rep.found = false;
    auto* eng = engine();
    if (!eng) return rep;
    auto c = exec_cmd(Op::GetTombstone);
    c.name = s(req.child_name);
    auto r = eng->call(std::move(c));
    rep.found     = r.tomb_found;
    rep.truncated = r.tomb_truncated;
    rep.total_bytes = static_cast<uint32_t>(
        std::min<uint64_t>(r.tomb_total, 0xFFFFFFFFull));
    std::snprintf(rep.path, sizeof(rep.path), "%s", r.tomb_path.c_str());
    // content is a fixed nanopb bytes char[] (size + max_size from .options).
    const size_t n = std::min(r.tomb_content.size(), sizeof(rep.content.bytes));
    std::memcpy(rep.content.bytes, r.tomb_content.data(), n);
    rep.content.size = static_cast<pb_size_t>(n);
    return rep;
}

ControlReply SupervisorCtl::handle_call(
        const ConfigureLogLevelRequest& req,
        SupervisorCtlState& st) {
    using Op = ::supervisor::ExecCommand::Op;
    const auto& cfg = req.config;
    const std::string target = s(cfg.target_node);
    ControlReply rep;

    // Self-target: `tdb loglevel root` sets the supervisor's OWN process log
    // level in-process via the framework's LogLevelPush handler — no engine
    // store, no resolve, no cast. (Same self redirect as ConfigureTrace.)
    if (is_self_target(target)) {
        this->handle_cast(cfg.log_level, st);  // base GenServer overload
        set_reply(rep, 0, target, "log level applied (supervisor self)");
        return rep;
    }

    auto* eng = engine();
    if (!eng) { set_reply(rep, 4, target, "engine not up"); return rep; }

    // STORE on the loop thread (survives restart) — ok=false if unresolved.
    auto c = exec_cmd(Op::ConfigureLogLevel);
    c.name  = target;
    c.level = static_cast<uint32_t>(cfg.log_level.level);
    if (!eng->call(std::move(c)).ok) {
        set_reply(rep, 4, target, "no worker or node by that name");
        return rep;
    }
    // Push live FROM THIS thread — resolve (loop) + cast (here), cfg.log_level
    // verbatim.
    ctl_set_log_level(target, cfg.log_level);
    set_reply(rep, 0, target, "log level applied");
    return rep;
}

LogLevelConfigList SupervisorCtl::handle_call(
        const GetLogLevelConfigRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    auto rows = eng ? eng->call(exec_cmd(Op::GetLogLevelConfig)).log_cfg
                    : std::vector<::supervisor::LogLevelRow>{};
    LogLevelConfigList list{};
    list.configs_count = 0;
    for (const auto& r : rows) {
        if (list.configs_count >=
            static_cast<pb_size_t>(sizeof(list.configs) / sizeof(list.configs[0]))) {
            break;  // fixed array full; truncate
        }
        auto& c = list.configs[list.configs_count++];
        c = system_supervisor_LogLevelStatus{};
        std::snprintf(c.target_node, sizeof(c.target_node), "%s",
                      r.target_node.c_str());
        c.level       = static_cast<platform_runtime_LogLevelValue>(r.level);
        c.is_override = r.is_override;
        c.boot_level  = static_cast<platform_runtime_LogLevelValue>(r.boot_level);
    }
    return list;
}

LoggerPolicy SupervisorCtl::handle_call(
        const GetLoggerPolicyRequest& /*req*/,
        SupervisorCtlState& /*s*/) {
    // The log[logging] hose's "what do I tail?" query — every supervised
    // worker's EXACT sink (file path / syslog tag) + the machine-level policy.
    // The engine reads each worker's THEIA_LOGGER spawn env (the value it
    // authored before execvp), so the answer is exact, not a guess.
    using Op = ::supervisor::ExecCommand::Op;
    auto* eng = engine();
    std::string machine_sink;
    std::vector<::supervisor::LoggerEntryRow> rows;
    if (eng) {
        auto rep = eng->call(exec_cmd(Op::GetLoggerPolicy));
        rows = std::move(rep.logger_cfg);
        machine_sink = std::move(rep.logger_machine_sink);
    }
    LoggerPolicy pol{};
    std::snprintf(pol.machine_sink, sizeof(pol.machine_sink), "%s",
                  machine_sink.c_str());
    pol.entries_count = 0;
    for (const auto& r : rows) {
        if (pol.entries_count >=
            static_cast<pb_size_t>(sizeof(pol.entries) / sizeof(pol.entries[0]))) {
            break;  // fixed array full; truncate
        }
        auto& e = pol.entries[pol.entries_count++];
        e = system_supervisor_LoggerEntry{};
        std::snprintf(e.node, sizeof(e.node), "%s", r.node.c_str());
        std::snprintf(e.sink, sizeof(e.sink), "%s", r.sink.c_str());
        std::snprintf(e.path, sizeof(e.path), "%s", r.path.c_str());
        std::snprintf(e.tag,  sizeof(e.tag),  "%s", r.tag.c_str());
    }
    return pol;
}


}  // namespace ara::exec
