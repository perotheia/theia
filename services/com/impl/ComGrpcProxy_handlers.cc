// User do_* bodies for the runnable node ComGrpcProxy.
//
// FIRST-TIME-ONLY SCAFFOLD origin: `artheia gen-app --kind fc`. Bodies are
// ours; the declarations live in lib/ComGrpcProxy.hh (byte-stable, do NOT add
// members there). All gRPC state is file-static in this translation unit —
// ComGrpcProxy is a single-instance runnable (one per com process), so a
// pimpl-via-file-statics keeps the generated header untouched.
//
// The old hand-rolled World-A gRPC main() folded into World B: this is now a
// first-class generated runnable node, built by the native Bazel binary
// //services/com/main:com (no more cmake / rules_foreign_cc). com's job:
// bridge external gRPC peers (dashboards / supdbg / rf-theia) to the on-host
// supervisor. We serve `SupervisorView` over gRPC.
//
// #418 — CONTROL path (the unary mutators: Start/Delete/Restart/Terminate/
// Stop/ConfigureLogLevel/ConfigureTrace/GetTraceConfig) goes over the STANDARD
// Theia transport: SupLink (impl/sup_link) holds a RemoteRef to the
// supervisor's gen_server control node (TIPC type 0x80020003/0) and does a
// typed nanopb CALL. Each RPC translates its libprotobuf gRPC args into the
// SupLink primitives, gets a SupReply back, and fills the libprotobuf reply.
//
// FIREHOSE path (Subscribe: live tree stream) is a GetTree POLL, not a push.
// The supervisor's in-process event firehose (broadcast_events_edge) has NO
// remote egress — it never crosses TIPC to com. So Subscribe mirrors
// `tdb ps --follow`: poll SupLink::get_tree() on an interval (THEIA_COM_POLL_MS,
// default 1s) and emit each TreeSnapshot as a `snapshot` observation. GetTree
// is the live source of truth — the same call `tdb ps` uses one-shot. The gRPC
// client diffs successive snapshots if it wants deltas; com does not fabricate
// event/health frames from a feed that never arrives. The gRPC edge stays
// libprotobuf; nanopb is confined to impl/sup_link.cc.
//
// BUILD NOTE: grpc++ + the supervisor bridge .pb/.grpc.pb come from native
// Bazel targets — the //services/com:com_bridge_grpc cc_library (host protoc +
// grpc_cpp_plugin genrule, grpc++ via system linkopts) and
// //platform/supervisor:supervisor_pb_cpp (the libprotobuf C++ bindings). The
// process therefore links BOTH libprotobuf (gRPC edge) AND libprotobuf-nanopb
// (ComDaemon's TIPC wire) — the deliberate two-codec, one-process design.

#include "lib/ComGrpcProxy.hh"

#include "impl/sup_link.hpp"      // #418 control path over the standard transport
#include "impl/per_link.hpp"      // per (persistency) proxy — ListSchemas/Snapshot
#include "impl/com_tls.hpp"       // shared TLS-or-insecure ServerCredentials

#include "supervisor_bridge.grpc.pb.h"

// The supervisor's wire types com bridges — now ONE consolidated libprotobuf
// header (package system_supervisor; ChildSelector, ChildSpec, ControlReply,
// SupervisionEvent, TraceConfigList, TreeSnapshot, …). The old per-message
// ChildSelector.pb.h / ControlRequest.pb.h / etc. are gone.
#include "supervisor.pb.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace ara::com {

namespace {

// op_kind values shared with platform/supervisor/src/runtime.cpp.
constexpr uint32_t kOpStartChild        = 3;
constexpr uint32_t kOpDeleteChild       = 4;
constexpr uint32_t kOpRestartChild      = 5;
constexpr uint32_t kOpTerminateChild    = 6;
constexpr uint32_t kOpConfigureTrace    = 9;   // #361 (trace control path)
constexpr uint32_t kOpGetTraceConfig    = 10;  // crash-investigation read-back
constexpr uint32_t kOpConfigureLogLevel = 11;  // #385

// Listen address. Overridable via THEIA_COM_LISTEN (the rig/executor sets it
// from the machine manifest's com gRPC endpoint); defaults to the historical
// 0.0.0.0:7700 that supervisor-gui + supdbg + rf-theia connect to.
std::string listen_addr() {
    if (const char* e = std::getenv("THEIA_COM_LISTEN")) return e;
    return "0.0.0.0:7700";
}

// ---- gRPC service: forwards control RPCs onto the supervisor uplink ------
class SupervisorViewImpl final
    : public services::com::SupervisorView::Service {
public:
    SupervisorViewImpl() = default;

    // ---- Streaming firehose — GetTree POLL (the pull model) --------------
    // The supervisor's in-process event firehose (broadcast_events_edge) has
    // no remote egress — it never reaches com over TIPC. So Subscribe mirrors
    // `tdb ps --follow`: poll GetTree on an interval and emit each TreeSnapshot
    // as a `snapshot` observation. GetTree is the live source of truth (same
    // call `tdb ps` uses one-shot). The client diffs successive snapshots if it
    // wants edge/health deltas; com no longer fabricates them from a dead feed.
    //
    // Interval: THEIA_COM_POLL_MS (default 1000ms), Ctrl-C/cancel-aware.
    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::com::SubscribeRequest*,
            grpc::ServerWriter<services::com::SupervisorObservation>* writer)
            override {
        int poll_ms = 1000;
        if (const char* e = std::getenv("THEIA_COM_POLL_MS")) {
            int v = std::atoi(e);
            if (v >= 50) poll_ms = v;
        }
        std::fprintf(stderr, "com: gRPC subscriber attached "
                     "(GetTree poll every %dms)\n", poll_ms);
        while (!ctx->IsCancelled()) {
            services_com::SupReply r;
            if (services_com::SupLink::instance().get_tree(r) &&
                !r.tree_snapshot.empty()) {
                services::com::SupervisorObservation obs;
                auto* s = obs.mutable_snapshot();
                if (s->ParseFromString(r.tree_snapshot)) {
                    if (!writer->Write(obs)) break;   // client gone
                }
            }
            // HealthBeacon: same poll cadence, separate observation (GUI Load
            // panel + "heartbeat" status). The supervisor's GetHealth returns
            // its latest beacon — no TIPC event-firehose subscription needed.
            services_com::SupReply hr;
            if (services_com::SupLink::instance().get_health(hr) &&
                !hr.health.empty()) {
                services::com::SupervisorObservation obs;
                auto* h = obs.mutable_health();
                if (h->ParseFromString(hr.health)) {
                    if (!writer->Write(obs)) break;
                }
            }
            // Sleep in short slices so cancellation is responsive.
            for (int slept = 0; slept < poll_ms && !ctx->IsCancelled();
                 slept += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min(100, poll_ms - slept)));
            }
        }
        std::fprintf(stderr, "com: gRPC subscriber detached\n");
        return grpc::Status::OK;
    }

    // ---- Unary mutators — #418 over the standard transport via SupLink -----
    grpc::Status StartChild(grpc::ServerContext*,
                            const services::com::StartChildCall* req,
                            system_supervisor::ControlReply* reply) override {
        const auto& gs = req->spec();
        services_com::SupChildSpec spec;
        spec.name              = gs.name();
        spec.parent_supervisor = gs.parent_supervisor();
        spec.restart           = gs.restart();
        spec.shutdown          = gs.shutdown();
        spec.type              = gs.type();
        for (const auto& a : gs.start_cmd()) spec.start_cmd.push_back(a);
        for (const auto& m : gs.modules())   spec.modules.push_back(m);
        services_com::SupReply r;
        if (!services_com::SupLink::instance().start_child(spec, r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }
    grpc::Status DeleteChild(grpc::ServerContext*,
                             const services::com::DeleteChildCall* req,
                             system_supervisor::ControlReply* reply) override {
        return name_op(kOpDeleteChild, req->name(), reply);
    }
    grpc::Status RestartChild(grpc::ServerContext*,
                              const ::system_supervisor::ChildSelector* sel,
                              system_supervisor::ControlReply* reply) override {
        return name_op(kOpRestartChild, sel->name(), reply);
    }
    grpc::Status TerminateChild(grpc::ServerContext*,
                                const ::system_supervisor::ChildSelector* sel,
                                system_supervisor::ControlReply* reply) override {
        return name_op(kOpTerminateChild, sel->name(), reply);
    }

    // #385 — set a child's runtime log level. The supervisor stores it
    // (survives restart) and pushes it live. Now over the standard transport.
    grpc::Status ConfigureLogLevel(
            grpc::ServerContext*,
            const services::com::LogLevelCall* req,
            system_supervisor::ControlReply* reply) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().configure_log_level(
                req->target_node(), req->level(), r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }

    // ConfigureTrace — the trace CONTROL/ingress path (rf → com → supervisor
    // → node). The supervisor owns trace_config_[child] + the per-NODE push
    // (#361), so it survives restart. com is NOT in the trace EGRESS byte path
    // — records stream from the collector's own TraceStream gRPC (services/log).
    grpc::Status ConfigureTrace(
            grpc::ServerContext*,
            const services::com::TraceConfigRequest* req,
            system_supervisor::ControlReply* reply) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().configure_trace(
                req->target_node(), req->msg_type(), req->enabled(),
                req->kind(), r))   // #403: trace-kind selector → node
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }

    // Crash-investigation read-back. The supervisor carries its flattened
    // TraceConfigList INLINE in ControlReply.trace_config_list (single
    // correlated frame). We get the raw proto bytes back from SupLink and
    // unpack them into the caller's libprotobuf TraceConfigList.
    grpc::Status GetTraceConfig(
            grpc::ServerContext*,
            const services::com::GetTraceConfigCall*,
            system_supervisor::TraceConfigList* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_trace_config(r))
            return unavailable();
        // Empty list (no trace configured) is a valid, non-error result.
        if (!r.trace_config_list.empty() &&
            !out->ParseFromString(r.trace_config_list)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed TraceConfigList in ControlReply");
        }
        return grpc::Status::OK;
    }

    // Host facts read-back — backs `rtdb supervisor` / `info`. The supervisor
    // serves SystemInfo over TIPC; we get the raw bytes from SupLink and
    // unpack into the caller's libprotobuf SystemInfo.
    grpc::Status GetSystemInfo(
            grpc::ServerContext*,
            const services::com::GetSystemInfoCall*,
            system_supervisor::SystemInfo* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_system_info(r))
            return unavailable();
        if (!r.system_info.empty() && !out->ParseFromString(r.system_info)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed SystemInfo from supervisor");
        }
        return grpc::Status::OK;
    }

    // Per-child log-level read-back — backs `rtdb loglevel` (no level arg).
    grpc::Status GetLogLevelConfig(
            grpc::ServerContext*,
            const services::com::GetLogLevelConfigCall*,
            system_supervisor::LogLevelConfigList* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_log_level_config(r))
            return unavailable();
        if (!r.log_level_list.empty() &&
            !out->ParseFromString(r.log_level_list)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed LogLevelConfigList from supervisor");
        }
        return grpc::Status::OK;
    }

    // Crash forensics — fetch a crashed child's tombstone bytes (capped at the
    // supervisor). The reply is com's OWN native message, so we copy SupReply
    // fields straight in (no proto-bytes round-trip). `found=false` is a valid,
    // non-error result (the child never crashed / no tombstone on disk).
    grpc::Status GetTombstone(
            grpc::ServerContext*,
            const services::com::GetTombstoneCall* req,
            services::com::GetTombstoneReply* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_tombstone(req->child_name(), r))
            return unavailable();
        out->set_found(r.tomb_found);
        out->set_path(r.tomb_path);
        out->set_truncated(r.tomb_truncated);
        out->set_total_bytes(r.tomb_total);
        out->set_content(r.tomb_content);
        return grpc::Status::OK;
    }

private:
    static grpc::Status unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "supervisor control link unavailable / timeout");
    }
    static void fill(::system_supervisor::ControlReply* out,
                     const services_com::SupReply& r) {
        out->set_status(r.status);
        out->set_message(r.message);
        out->set_child_name(r.child_name);
        if (!r.trace_config_list.empty())
            out->set_trace_config_list(r.trace_config_list);
    }
    grpc::Status name_op(uint32_t op_kind, const std::string& name,
                         ::system_supervisor::ControlReply* reply) {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().name_op(op_kind, name, r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }
    // No firehose member: Subscribe POLLS SupLink::get_tree() (the pull model).
};

// ---- gRPC service: PerView — proxy services/per's manager ops -------------
// com proxies per's schema-registry + snapshot ops over gRPC (so the GUI/rtdb
// inspect the config store without a second etcd client). PerLink RemoteRef-
// calls PerManager (TIPC 0x80010016); this edge translates per's primitive
// PerSchema/PerOpReply ↔ the libprotobuf PerView messages.
class PerViewImpl final : public services::com::PerView::Service {
public:
    grpc::Status ListSchemas(
            grpc::ServerContext*,
            const services::com::ListSchemasCall* req,
            services::com::PerSchemaList* out) override {
        std::vector<services_com::PerSchema> schemas;
        if (!services_com::PerLink::instance().list_schemas(
                req ? req->config_type() : "", schemas))
            return per_unavailable();
        for (const auto& s : schemas) {
            auto* row = out->add_schemas();
            row->set_config_type(s.config_type);
            row->set_digest(s.digest);
        }
        return grpc::Status::OK;
    }

    grpc::Status Snapshot(
            grpc::ServerContext*,
            const services::com::SnapshotCall* req,
            services::com::PerReply* out) override {
        services_com::PerOpReply r;
        if (!services_com::PerLink::instance().snapshot(
                req ? req->label() : "", r))
            return per_unavailable();
        out->set_status(r.status);
        out->set_message(r.message);
        out->set_mod_rev(r.mod_rev);
        return grpc::Status::OK;
    }

private:
    static grpc::Status per_unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "persistency link (per) unavailable / timeout");
    }
};

// ---- File-static runnable state (one ComGrpcProxy per process) -----------
// Held here rather than as ComGrpcProxy members so lib/ComGrpcProxy.hh stays
// byte-stable against gen-app. do_start builds them; do_stop tears them down.
// No firehose uplink: the live tree is a GetTree poll (SupLink), so com needs
// no ComDaemon cast-sink wiring — Subscribe pulls on an interval.
std::unique_ptr<SupervisorViewImpl>         g_sup_svc;
std::unique_ptr<PerViewImpl>                g_per_svc;
std::unique_ptr<grpc::Server>               g_server;
std::atomic<bool>                           g_up{false};

}  // namespace

// One-time setup on the worker thread, before do_loop(): open the supervisor
// TIPC uplink, then build + start the gRPC SupervisorView server. If either
// fails we leave g_up false so do_loop() falls straight through and the
// runnable exits — the supervisor's watchdog then restarts com.
void ComGrpcProxy::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    // #418 — control link over the standard transport (RemoteRef → SupervisorCtl
    // at 0x80020001/0). Best-effort: if the supervisor isn't reachable yet,
    // control RPCs (and the Subscribe GetTree poll) return UNAVAILABLE until a
    // restart. We do NOT hard-fail do_start on it.
    if (!services_com::SupLink::instance().start()) {
        std::fprintf(stderr,
                     "[%s] WARN: supervisor control link (RemoteRef "
                     "0x80020001/0) not reachable; control RPCs will return "
                     "UNAVAILABLE until it connects\n", kNodeName);
    } else {
        std::fprintf(stderr,
                     "[%s] supervisor control link up (RemoteRef 0x80020001/0)\n",
                     kNodeName);
    }

    // per (persistency) proxy link — RemoteRef → PerManager (0x80010016).
    // Best-effort like the supervisor link: PerView RPCs return UNAVAILABLE
    // until per is reachable. (per may be down or absent on a machine.)
    if (!services_com::PerLink::instance().start()) {
        std::fprintf(stderr,
                     "[%s] WARN: persistency link (per 0x80010016) not "
                     "reachable; PerView RPCs return UNAVAILABLE\n", kNodeName);
    } else {
        std::fprintf(stderr,
                     "[%s] persistency link up (RemoteRef 0x80010016/0)\n",
                     kNodeName);
    }

    g_sup_svc = std::make_unique<SupervisorViewImpl>();
    g_per_svc = std::make_unique<PerViewImpl>();

    const std::string listen = listen_addr();
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, services_com::make_server_creds(kNodeName));
    b.RegisterService(g_sup_svc.get());
    b.RegisterService(g_per_svc.get());
    g_server = b.BuildAndStart();
    if (!g_server) {
        std::fprintf(stderr, "[%s] gRPC server failed to start on %s\n",
                     kNodeName, listen.c_str());
        g_sup_svc.reset();
        g_per_svc.reset();
        services_com::SupLink::instance().stop();
        services_com::PerLink::instance().stop();
        return;
    }
    std::fprintf(stderr,
                 "[%s] gRPC SupervisorView + PerView listening on %s\n",
                 kNodeName, listen.c_str());
    g_up.store(true);
}

// The body. grpc::Server runs its own thread pool from BuildAndStart(), so
// this loop just parks until stop_requested() (do_stop wakes the server). A
// non-reporting runnable (kReporting==false), so no watchdog beat is needed —
// a plain cooperative poll is correct here.
void ComGrpcProxy::do_loop() {
    if (!g_up.load()) {
        std::fprintf(stderr, "[%s] startup failed; loop exiting immediately\n",
                     kNodeName);
        return;
    }
    while (!stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "[%s] runnable loop exiting\n", kNodeName);
}

// Release + signal do_loop() to return: drain the gRPC server (bounded
// deadline so in-flight RPCs finish), then close the supervisor uplink.
void ComGrpcProxy::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    if (g_server) {
        g_server->Shutdown(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(500));
        g_server.reset();
    }
    g_sup_svc.reset();
    g_per_svc.reset();
    services_com::SupLink::instance().stop();
    services_com::PerLink::instance().stop();
    g_up.store(false);
}

}  // namespace ara::com
