// services/com — supervisor-bridge gRPC server.
//
// External clients (supervisor-gui, dashboards) connect over gRPC.
// We forward observations from the in-host supervisor (over TIPC) as
// a server-streaming Subscribe() RPC, and translate unary control
// RPCs (StartChild / DeleteChild / RestartChild / TerminateChild)
// into ControlRequest frames sent over the same TIPC link.

#include "com/tipc_uplink.hpp"
#include "com/tipc_trace_uplink.hpp"

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
constexpr uint32_t kOpStartChild     = 3;
constexpr uint32_t kOpDeleteChild    = 4;
constexpr uint32_t kOpRestartChild   = 5;
constexpr uint32_t kOpTerminateChild = 6;

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


// ---------------------------------------------------------------------------
// TraceStreamImpl (#354) — gRPC bridge over TipcTraceUplink.
//
// Sister to SupervisorViewImpl: opens one TIPC connection to
// services/log[trace] (TraceCollector node, TIPC type 0x80010013)
// and serves Subscribe() / Configure() over gRPC. Subscribers attach
// to the uplink and receive every TraceRecord landing on the
// collector's fanout; Configure forwards a TraceConfigRequest down
// the same TIPC link.
// ---------------------------------------------------------------------------
class TraceStreamImpl final
    : public services::com::TraceStream::Service {
public:
    explicit TraceStreamImpl(services_com::TipcTraceUplink* uplink)
        : uplink_(uplink) {}

    grpc::Status Subscribe(grpc::ServerContext* ctx,
                           const services::com::TraceSubscribeRequest*,
                           grpc::ServerWriter<services::com::TraceRecord>* writer) override {
        if (!uplink_->running()) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "trace uplink not connected to log[trace]");
        }
        auto sub = uplink_->subscribe();
        std::fprintf(stderr, "com: gRPC trace subscriber attached\n");
        while (!ctx->IsCancelled()) {
            services_com::TraceFrame f;
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
            services::com::TraceRecord rec;
            if (!rec.ParseFromString(f.payload)) {
                // Malformed wire frame — log + drop, don't kill the stream.
                std::fprintf(stderr,
                    "com: trace: dropping malformed payload (%zu bytes)\n",
                    f.payload.size());
                continue;
            }
            if (!writer->Write(rec)) break;     // client gone
        }
        uplink_->unsubscribe(sub);
        std::fprintf(stderr, "com: gRPC trace subscriber detached\n");
        return grpc::Status::OK;
    }

    grpc::Status Configure(grpc::ServerContext*,
                           const services::com::TraceConfigRequest* req,
                           services::supervisor::ControlReply* reply) override {
        if (!uplink_->running()) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "trace uplink not connected to log[trace]");
        }
        // Wire format on the TIPC link is the same TraceConfigRequest
        // proto — TraceCollector parses it and forwards to the
        // supervisor's NodeTraceCtl push (#361). Fire-and-forget; no
        // synchronous reply on the TIPC side. ControlReply status=0
        // means "accepted by bridge", not "applied at FC".
        bool ok = uplink_->send_config_request(req->SerializeAsString());
        if (!ok) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "trace uplink send failed");
        }
        reply->set_status(0);
        reply->set_message("trace config queued");
        return grpc::Status::OK;
    }

private:
    services_com::TipcTraceUplink* uplink_;
};


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

    // The trace uplink is OPTIONAL — services/log[trace] may not be
    // running yet (especially on first bringup). If it fails we still
    // serve SupervisorView; trace RPCs return UNAVAILABLE until the
    // uplink connects (no auto-retry in this iteration; restart com
    // after log[trace] comes up).
    services_com::TipcTraceUplink trace_uplink;
    if (!trace_uplink.start()) {
        std::fprintf(stderr, "com: trace uplink not connected — "
                     "TraceStream RPCs will return UNAVAILABLE. "
                     "is services/log[trace] running?\n");
    }

    SupervisorViewImpl  sup_svc(&uplink);
    TraceStreamImpl     trace_svc(&trace_uplink);
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, grpc::InsecureServerCredentials());
    b.RegisterService(&sup_svc);
    b.RegisterService(&trace_svc);
    std::unique_ptr<grpc::Server> server(b.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "com: gRPC server failed to start on %s\n",
                     listen.c_str());
        return 2;
    }
    std::fprintf(stderr, "com: gRPC SupervisorView+TraceStream listening on %s\n",
                 listen.c_str());

    // Block until signal.
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "com: shutting down\n");
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));
    trace_uplink.stop();
    uplink.stop();
    return 0;
}
