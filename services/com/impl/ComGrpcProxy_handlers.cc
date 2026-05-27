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
// FIREHOSE path (Subscribe: event/health/snapshot fan-out) now rides the
// STANDARD transport too (#432): the supervisor CASTs a #429 topo-pair stream
// (SnapshotBegin/NodeEdge/NodeState/SnapshotEnd) to com's ComDaemon (TIPC
// 0x80010008/0); main.cc register_casts the four service_ids and feeds the
// decoded values to SupFirehose (impl/sup_firehose), which REASSEMBLES a
// libprotobuf TreeSnapshot and fans it out here as the SAME tagged Frame the
// Subscribe loop already consumes. The legacy TipcUplink reader on the
// publisher socket (0x80020001) is RETIRED. The gRPC edge stays libprotobuf;
// the nanopb world is confined to impl/sup_link.cc + main.cc's decode.
//
// BUILD NOTE: grpc++ + the supervisor bridge .pb/.grpc.pb come from native
// Bazel targets — the //services/com:com_bridge_grpc cc_library (host protoc +
// grpc_cpp_plugin genrule, grpc++ via system linkopts) and
// //platform/supervisor:supervisor_pb_cpp (the libprotobuf C++ bindings). The
// process therefore links BOTH libprotobuf (gRPC edge) AND libprotobuf-nanopb
// (ComDaemon's TIPC wire) — the deliberate two-codec, one-process design.

#include "lib/ComGrpcProxy.hh"

#include "impl/sup_firehose.hpp"  // #432 firehose: ComDaemon-fed topo-pair stream
#include "impl/sup_link.hpp"      // #418 control path over the standard transport

#include "supervisor_bridge.grpc.pb.h"

// supervisor's own protos — reused verbatim (the wire types com bridges).
#include "ChildSelector.pb.h"
#include "ChildSpec.pb.h"
#include "ControlReply.pb.h"
#include "ControlRequest.pb.h"
#include "DeleteChildRequest.pb.h"
#include "HealthBeacon.pb.h"
#include "StartChildRequest.pb.h"
#include "SupervisionEvent.pb.h"
#include "TraceConfig.pb.h"
#include "TraceConfigList.pb.h"
#include "TreeSnapshot.pb.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

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

// ---- Frame tags (shared with TipcUplink's reader) ------------------------
constexpr uint16_t kTagEvent    = 0x0001;
constexpr uint16_t kTagHealth   = 0x0002;
constexpr uint16_t kTagSnapshot = 0x0003;

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

    // ---- Streaming firehose ----------------------------------------------
    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::com::SubscribeRequest*,
            grpc::ServerWriter<services::com::SupervisorObservation>* writer)
            override {
        auto sub = services_com::SupFirehose::instance().subscribe();
        std::fprintf(stderr, "com: gRPC subscriber attached\n");
        while (!ctx->IsCancelled()) {
            services_com::Frame f;
            {
                std::unique_lock<std::mutex> lk(sub->mtx);
                sub->cv.wait_for(lk, std::chrono::milliseconds(200),
                                 [&] {
                                     return sub->closed || !sub->queue.empty();
                                 });
                if (sub->closed && sub->queue.empty()) break;
                if (sub->queue.empty()) continue;
                f = std::move(sub->queue.front());
                sub->queue.pop_front();
            }
            services::com::SupervisorObservation obs;
            switch (f.tag) {
                case kTagEvent: {
                    auto* e = obs.mutable_event();
                    if (!e->ParseFromString(f.payload)) continue;
                    break;
                }
                case kTagHealth: {
                    auto* h = obs.mutable_health();
                    if (!h->ParseFromString(f.payload)) continue;
                    break;
                }
                case kTagSnapshot: {
                    auto* s = obs.mutable_snapshot();
                    if (!s->ParseFromString(f.payload)) continue;
                    break;
                }
                default:
                    continue;          // unknown — skip
            }
            if (!writer->Write(obs)) break;        // client gone
        }
        services_com::SupFirehose::instance().unsubscribe(sub);
        std::fprintf(stderr, "com: gRPC subscriber detached\n");
        return grpc::Status::OK;
    }

    // ---- Unary mutators — #418 over the standard transport via SupLink -----
    grpc::Status StartChild(grpc::ServerContext*,
                            const services::com::StartChildCall* req,
                            services::supervisor::ControlReply* reply) override {
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
                             services::supervisor::ControlReply* reply) override {
        return name_op(kOpDeleteChild, req->name(), reply);
    }
    grpc::Status RestartChild(grpc::ServerContext*,
                              const ::services::supervisor::ChildSelector* sel,
                              services::supervisor::ControlReply* reply) override {
        return name_op(kOpRestartChild, sel->name(), reply);
    }
    grpc::Status TerminateChild(grpc::ServerContext*,
                                const ::services::supervisor::ChildSelector* sel,
                                services::supervisor::ControlReply* reply) override {
        return name_op(kOpTerminateChild, sel->name(), reply);
    }

    // #385 — set a child's runtime log level. The supervisor stores it
    // (survives restart) and pushes it live. Now over the standard transport.
    grpc::Status ConfigureLogLevel(
            grpc::ServerContext*,
            const services::com::LogLevelCall* req,
            services::supervisor::ControlReply* reply) override {
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
            services::supervisor::ControlReply* reply) override {
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
            services::supervisor::TraceConfigList* out) override {
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

private:
    static grpc::Status unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "supervisor control link unavailable / timeout");
    }
    static void fill(::services::supervisor::ControlReply* out,
                     const services_com::SupReply& r) {
        out->set_status(r.status);
        out->set_message(r.message);
        out->set_child_name(r.child_name);
        if (!r.trace_config_list.empty())
            out->set_trace_config_list(r.trace_config_list);
    }
    grpc::Status name_op(uint32_t op_kind, const std::string& name,
                         ::services::supervisor::ControlReply* reply) {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().name_op(op_kind, name, r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }
    // No firehose member: Subscribe reads from SupFirehose::instance() (#432).
};

// ---- File-static runnable state (one ComGrpcProxy per process) -----------
// Held here rather than as ComGrpcProxy members so lib/ComGrpcProxy.hh stays
// byte-stable against gen-app. do_start builds them; do_stop tears them down.
// No firehose uplink anymore (#432): the firehose arrives over the runtime
// via ComDaemon → SupFirehose, which Subscribe reads from directly.
std::unique_ptr<SupervisorViewImpl>         g_sup_svc;
std::unique_ptr<grpc::Server>               g_server;
std::atomic<bool>                           g_up{false};

}  // namespace

// One-time setup on the worker thread, before do_loop(): open the supervisor
// TIPC uplink, then build + start the gRPC SupervisorView server. If either
// fails we leave g_up false so do_loop() falls straight through and the
// runnable exits — the supervisor's watchdog then restarts com.
void ComGrpcProxy::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    // #432 — the firehose no longer needs a connect-or-fail uplink: the
    // supervisor CASTs the #429 topo-pair stream to ComDaemon (bound by
    // main.cc) which feeds SupFirehose; Subscribe reads from it. So do_start
    // no longer hard-fails when the supervisor is down — Subscribe just sees
    // no frames until the supervisor comes up and casts a snapshot.

    // #418 — control link over the standard transport (RemoteRef → control
    // node at 0x80020003/0). Best-effort: if the supervisor's control node
    // isn't reachable yet, control RPCs return UNAVAILABLE until a restart,
    // but the firehose still serves. We do NOT hard-fail do_start on it.
    if (!services_com::SupLink::instance().start()) {
        std::fprintf(stderr,
                     "[%s] WARN: supervisor control link (RemoteRef "
                     "0x80020003/0) not reachable; control RPCs will return "
                     "UNAVAILABLE until it connects\n", kNodeName);
    } else {
        std::fprintf(stderr,
                     "[%s] supervisor control link up (RemoteRef 0x80020003/0)\n",
                     kNodeName);
    }

    g_sup_svc = std::make_unique<SupervisorViewImpl>();

    const std::string listen = listen_addr();
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, grpc::InsecureServerCredentials());
    b.RegisterService(g_sup_svc.get());
    g_server = b.BuildAndStart();
    if (!g_server) {
        std::fprintf(stderr, "[%s] gRPC server failed to start on %s\n",
                     kNodeName, listen.c_str());
        g_sup_svc.reset();
        services_com::SupLink::instance().stop();
        return;
    }
    std::fprintf(stderr, "[%s] gRPC SupervisorView listening on %s\n",
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
    services_com::SupLink::instance().stop();
    g_up.store(false);
}

}  // namespace ara::com
