// services/com — supervisor-bridge gRPC server.
//
// External clients (supervisor-gui, dashboards) connect over gRPC.
// We forward observations from the in-host supervisor (over TIPC) as
// a server-streaming Subscribe() RPC, and translate unary control
// RPCs (StartChild / DeleteChild / RestartChild / TerminateChild)
// into ControlRequest frames sent over the same TIPC link.

#include "com/tipc_uplink.hpp"
#ifdef THEIA_ROBOT_NODE
#include "com/robot_node.hpp"   // test-only signal inject + service call (#387)
#endif

#include "supervisor_bridge.grpc.pb.h"

// supervisor's own protos — reused verbatim.
#include "ChildSelector.pb.h"
#include "ChildSpec.pb.h"
#include "ControlReply.pb.h"
#include "ControlRequest.pb.h"
#include "DeleteChildRequest.pb.h"
#include "HealthBeacon.pb.h"
#include "StartChildRequest.pb.h"
#include "SupervisionEvent.pb.h"
#include "TreeSnapshot.pb.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kTagEvent    = 0x0001;
constexpr uint16_t kTagHealth   = 0x0002;
constexpr uint16_t kTagSnapshot = 0x0003;

// op_kind values shared with platform/supervisor/src/runtime.cpp.
constexpr uint32_t kOpStartChild       = 3;
constexpr uint32_t kOpDeleteChild      = 4;
constexpr uint32_t kOpRestartChild     = 5;
constexpr uint32_t kOpTerminateChild   = 6;
constexpr uint32_t kOpConfigureTrace   = 9;   // #361 (trace control path)
constexpr uint32_t kOpConfigureLogLevel = 11;  // #385

std::atomic<bool> g_shutdown{false};
void on_signal(int) { g_shutdown.store(true); }

}  // namespace

class SupervisorViewImpl final
    : public services::com::SupervisorView::Service {
public:
    explicit SupervisorViewImpl(services_com::TipcUplink* uplink)
        : uplink_(uplink) {}

    // ---- Streaming firehose --------------------------------------------------
    grpc::Status Subscribe(grpc::ServerContext* ctx,
                            const services::com::SubscribeRequest*,
                            grpc::ServerWriter<services::com::SupervisorObservation>* writer) override {
        auto sub = uplink_->subscribe();
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
        uplink_->unsubscribe(sub);
        std::fprintf(stderr, "com: gRPC subscriber detached\n");
        return grpc::Status::OK;
    }

    // ---- Unary mutators ------------------------------------------------------
    grpc::Status StartChild(grpc::ServerContext*,
                             const services::com::StartChildCall* req,
                             services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpStartChild);
        cr.set_correlation_id(uplink_->next_correlation_id());
        *cr.mutable_start_child()->mutable_spec() = req->spec();
        return forward(cr, reply);
    }
    grpc::Status DeleteChild(grpc::ServerContext*,
                              const services::com::DeleteChildCall* req,
                              services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpDeleteChild);
        cr.set_correlation_id(uplink_->next_correlation_id());
        cr.mutable_delete_child()->set_name(req->name());
        return forward(cr, reply);
    }
    grpc::Status RestartChild(grpc::ServerContext*,
                               const ::services::supervisor::ChildSelector* sel,
                               services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpRestartChild);
        cr.set_correlation_id(uplink_->next_correlation_id());
        *cr.mutable_restart_child() = *sel;
        return forward(cr, reply);
    }
    grpc::Status TerminateChild(grpc::ServerContext*,
                                 const ::services::supervisor::ChildSelector* sel,
                                 services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpTerminateChild);
        cr.set_correlation_id(uplink_->next_correlation_id());
        *cr.mutable_terminate_child() = *sel;
        return forward(cr, reply);
    }

    // #385 — set a child's runtime log level. Packages (target_node,
    // level) into the supervisor's ConfigureLogLevelRequest and forwards
    // over the same ControlRequest envelope as the child mutators. The
    // supervisor stores it (survives restart) and pushes it live.
    grpc::Status ConfigureLogLevel(
            grpc::ServerContext*,
            const services::com::LogLevelCall* req,
            services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpConfigureLogLevel);
        cr.set_correlation_id(uplink_->next_correlation_id());
        auto* cfg = cr.mutable_configure_log_level()->mutable_config();
        cfg->set_target_node(req->target_node());
        cfg->set_level(req->level());
        return forward(cr, reply);
    }

    // ConfigureTrace — the trace CONTROL/ingress path (rf → com →
    // supervisor → node). Routes through the supervisor (op_kind=9,
    // kOpConfigureTrace) which owns trace_config_[child] + the per-NODE
    // push (#361), so it survives restart. com is NOT in the trace EGRESS
    // byte path — records stream from the collector's own TraceStream
    // gRPC (services/log). Relocated here when TraceStream moved out of
    // com (egress-direct design).
    grpc::Status ConfigureTrace(
            grpc::ServerContext*,
            const services::com::TraceConfigRequest* req,
            services::supervisor::ControlReply* reply) override {
        ::services::supervisor::ControlRequest cr;
        cr.set_op_kind(kOpConfigureTrace);
        cr.set_correlation_id(uplink_->next_correlation_id());
        auto* cfg = cr.mutable_configure_trace()->mutable_config();
        cfg->set_target_node(req->target_node());
        cfg->set_msg_type(req->msg_type());
        cfg->set_enabled(req->enabled());
        cfg->set_kind(req->kind());   // #403: trace-kind selector → node
        return forward(cr, reply);
    }

#ifdef THEIA_ROBOT_NODE
    // ---- Robot node (#387) — test-only signal inject + service call ----
    // com opens a direct TIPC client to the target node and speaks the
    // standard GW_MSG_GEN_CAST / GW_MSG_GEN_CALL wire shape. The test
    // built `payload` host-side with the std python protobuf lib and
    // resolved the target's TIPC addr from its rig context.
    grpc::Status InjectSignal(
            grpc::ServerContext*,
            const services::com::InjectSignalCall* req,
            services::com::InjectSignalAck* ack) override {
        bool sent = services_com::robot_inject_signal(
            req->tipc_type(), req->tipc_instance(),
            req->msg_type(), req->payload());
        ack->set_sent(sent);
        ack->set_message(sent ? "cast sent"
                              : "TIPC connect/send failed (node down?)");
        return grpc::Status::OK;
    }

    grpc::Status CallService(
            grpc::ServerContext*,
            const services::com::CallServiceCall* req,
            services::com::CallServiceReply* reply) override {
        auto r = services_com::robot_call_service(
            req->tipc_type(), req->tipc_instance(),
            req->req_msg_type(), req->payload(),
            static_cast<int>(req->timeout_ms()));
        reply->set_ok(r.ok);
        reply->set_message(r.ok ? "reply received" : r.error);
        if (r.ok) reply->set_payload(r.reply_payload);
        return grpc::Status::OK;
    }
#endif  // THEIA_ROBOT_NODE

private:
    grpc::Status forward(const ::services::supervisor::ControlRequest& cr,
                          ::services::supervisor::ControlReply* out) {
        std::string reply_payload;
        bool ok = uplink_->send_control_request(
            cr.SerializeAsString(), cr.correlation_id(), reply_payload);
        if (!ok) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "supervisor reply timeout");
        }
        if (!out->ParseFromString(reply_payload)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed ControlReply");
        }
        return grpc::Status::OK;
    }

    services_com::TipcUplink* uplink_;
};


// NOTE: the TraceStream gRPC bridge (#354) was REMOVED from com — com is
// a gRPC↔art-protocol proxy, not a trace byte-pump. Trace EGRESS is served
// directly by the collector's own TraceStream gRPC (services/log); the
// trace CONTROL path stays here as SupervisorView.ConfigureTrace (above).


int main(int argc, char** argv) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    std::string listen = "0.0.0.0:7700";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen" && i + 1 < argc) {
            listen = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "usage: %s [--listen HOST:PORT]   (default 0.0.0.0:7700)\n",
                argv[0]);
            return 0;
        }
    }

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    services_com::TipcUplink uplink;
    if (!uplink.start()) {
        std::fprintf(stderr, "com: failed to connect to supervisor via TIPC; "
                     "is the supervisor running?\n");
        return 1;
    }

    SupervisorViewImpl  sup_svc(&uplink);
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, grpc::InsecureServerCredentials());
    b.RegisterService(&sup_svc);
    std::unique_ptr<grpc::Server> server(b.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "com: gRPC server failed to start on %s\n",
                     listen.c_str());
        return 2;
    }
    std::fprintf(stderr, "com: gRPC SupervisorView listening on %s\n",
                 listen.c_str());

    // Block until signal.
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "com: shutting down\n");
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));
    uplink.stop();
    return 0;
}
