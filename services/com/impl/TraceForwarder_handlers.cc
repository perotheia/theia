// TraceForwarder — the trace EGRESS gRPC thread (Phase C).
//
// World-B counterpart to ComGrpcProxy: a runnable that owns a gRPC server, but
// for TRACE records instead of supervisor control. It subscribes ONCE to
// log[trace]'s TIPC hub (via TraceLink) and fans every record out to all
// gRPC TraceStream subscribers. log[trace] stays a pure-TIPC hub — the old
// services/log gRPC daemon is retired.
//
// Two codecs, isolated like ComGrpcProxy: TraceLink (impl/trace_link.cc) owns
// the nanopb/TIPC side and hands us RAW record bytes; this TU is libprotobuf
// only — it ParseFromString's those bytes into services.com.TraceRecord (the
// shapes are byte-identical) and writes them to the gRPC stream.

#include "lib/TraceForwarder.hh"

#include "impl/trace_link.hpp"   // TraceLink: TIPC subscriber → raw-bytes sink

#include "supervisor_bridge.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace ara::com {

namespace {

// The trace egress listens on its OWN port (distinct from ComGrpcProxy's
// SupervisorView :7700) so the two runnables are independently restartable.
std::string trace_listen_addr() {
    if (const char* e = std::getenv("THEIA_COM_TRACE_LISTEN")) return e;
    return "0.0.0.0:7710";
}

// One gRPC subscriber: a bounded queue + a CV. TraceLink's sink pushes raw
// record bytes onto every attached subscriber; each Subscribe RPC drains its
// own queue to its writer. A slow/dead client's queue is capped (drop-oldest)
// so it can't back up the single TIPC recv thread.
struct GrpcSub {
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;     // raw TraceRecord proto bytes
    bool                    closed = false;
    static constexpr size_t kMaxQueued = 4096;
};

// The live subscriber set. The TraceLink sink (TIPC recv thread) iterates it
// under reg_mtx and pushes to each; Subscribe RPCs add/remove themselves.
std::mutex                              g_reg_mtx;
std::unordered_set<std::shared_ptr<GrpcSub>> g_subs;

void fanout(const std::string& record_bytes) {
    std::lock_guard<std::mutex> rl(g_reg_mtx);
    for (auto& sub : g_subs) {
        std::lock_guard<std::mutex> lk(sub->mtx);
        if (sub->queue.size() >= GrpcSub::kMaxQueued)
            sub->queue.pop_front();        // drop-oldest; never block recv
        sub->queue.push_back(record_bytes);
        sub->cv.notify_one();
    }
}

// ---- gRPC service: TraceStream.Subscribe ---------------------------------
class TraceStreamImpl final : public services::com::TraceStream::Service {
public:
    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::com::TraceSubscribeRequest* req,
            grpc::ServerWriter<services::com::TraceRecord>* writer) override {
        // Per-stream filters (kind / target_node) are applied here — the TIPC
        // hub fan-out is unfiltered; selection is cheap stream-side.
        const uint32_t kind_filter = req ? req->kind() : 0;
        const std::string node_filter = req ? req->target_node() : "";

        auto sub = std::make_shared<GrpcSub>();
        {
            std::lock_guard<std::mutex> rl(g_reg_mtx);
            g_subs.insert(sub);
        }
        std::fprintf(stderr, "com: gRPC trace subscriber attached "
                     "(kind=%u node=%s)\n", kind_filter,
                     node_filter.empty() ? "*" : node_filter.c_str());

        while (!ctx->IsCancelled()) {
            std::string raw;
            {
                std::unique_lock<std::mutex> lk(sub->mtx);
                sub->cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                    return sub->closed || !sub->queue.empty();
                });
                if (sub->closed && sub->queue.empty()) break;
                if (sub->queue.empty()) continue;
                raw = std::move(sub->queue.front());
                sub->queue.pop_front();
            }
            services::com::TraceRecord rec;
            if (!rec.ParseFromString(raw)) continue;   // malformed — skip
            if (kind_filter != 0 && rec.kind() != kind_filter) continue;
            if (!node_filter.empty() && rec.node_name() != node_filter) continue;
            if (!writer->Write(rec)) break;            // client gone
        }
        {
            std::lock_guard<std::mutex> rl(g_reg_mtx);
            g_subs.erase(sub);
        }
        std::fprintf(stderr, "com: gRPC trace subscriber detached\n");
        return grpc::Status::OK;
    }
};

std::unique_ptr<TraceStreamImpl> g_svc;
std::unique_ptr<grpc::Server>    g_server;
std::atomic<bool>                g_up{false};

}  // namespace

// One-time setup: subscribe to the TIPC trace hub, then build + start the gRPC
// TraceStream server. If the hub isn't reachable yet we still serve gRPC (empty
// streams) — TraceLink::start best-effort; the supervisor restarts com if the
// gRPC server itself fails to bind.
void TraceForwarder::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    // The sink that bridges TIPC records → the gRPC subscriber fan-out. Set it
    // BEFORE start() so the backlog spill isn't dropped.
    services_com::TraceLink::instance().set_sink(
        [](const std::string& record_bytes) { fanout(record_bytes); });

    if (!services_com::TraceLink::instance().start()) {
        std::fprintf(stderr,
            "[%s] WARN: trace hub (TraceCtl) not reachable; trace streams "
            "will be empty until it connects\n", kNodeName);
    }

    g_svc = std::make_unique<TraceStreamImpl>();
    const std::string listen = trace_listen_addr();
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, grpc::InsecureServerCredentials());
    b.RegisterService(g_svc.get());
    g_server = b.BuildAndStart();
    if (!g_server) {
        std::fprintf(stderr, "[%s] gRPC trace server failed to start on %s\n",
                     kNodeName, listen.c_str());
        g_svc.reset();
        services_com::TraceLink::instance().stop();
        return;
    }
    std::fprintf(stderr, "[%s] gRPC TraceStream listening on %s\n",
                 kNodeName, listen.c_str());
    g_up.store(true);
}

void TraceForwarder::do_loop() {
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

void TraceForwarder::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    if (g_server) {
        g_server->Shutdown(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(500));
        g_server.reset();
    }
    g_svc.reset();
    services_com::TraceLink::instance().stop();
    // Wake any subscriber loops still parked on their CV.
    {
        std::lock_guard<std::mutex> rl(g_reg_mtx);
        for (auto& sub : g_subs) {
            std::lock_guard<std::mutex> lk(sub->mtx);
            sub->closed = true;
            sub->cv.notify_all();
        }
    }
    g_up.store(false);
}

}  // namespace ara::com
