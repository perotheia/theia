// LogForwarder — the log EGRESS gRPC thread.
//
// World-B counterpart to TraceForwarder, for LOG lines instead of trace
// records. A runnable that owns a gRPC server and subscribes ONCE to
// log[logging]'s TIPC hub (via LogLink), fanning every line out to all gRPC
// LogStream subscribers. log[logging] stays a pure-TIPC hub; com is just an
// egress bridge so rtdb logcat works from outside the DMZ.
//
// Two codecs, isolated like TraceForwarder: LogLink (impl/log_link.cc) owns the
// nanopb/TIPC side and hands us RAW record bytes; this TU is libprotobuf only —
// it ParseFromString's those bytes into services.com.LogRecord (the shapes are
// byte-identical) and writes them to the gRPC stream.

#include "lib/LogForwarder.hh"

#include "impl/log_link.hpp"   // LogLink: TIPC subscriber → raw-bytes sink
#include "impl/com_tls.hpp"    // shared TLS-or-insecure ServerCredentials

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

// The log egress listens on its OWN port (distinct from ComGrpcProxy :7700 and
// TraceForwarder :7710) so the three runnables are independently restartable.
std::string log_listen_addr() {
    if (const char* e = std::getenv("THEIA_COM_LOG_LISTEN")) return e;
    return "0.0.0.0:7711";
}

// One gRPC subscriber: a bounded queue + a CV. LogLink's sink pushes raw record
// bytes onto every attached subscriber; each Subscribe RPC drains its own queue
// to its writer. A slow/dead client's queue is capped (drop-oldest) so it can't
// back up the single TIPC recv thread.
struct GrpcSub {
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;     // raw LogRecord proto bytes
    bool                    closed = false;
    static constexpr size_t kMaxQueued = 4096;
};

std::mutex                                   g_reg_mtx;
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

// ---- gRPC service: LogStream.Subscribe -----------------------------------
class LogStreamImpl final : public services::com::LogStream::Service {
public:
    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::com::LogSubscribeRequest* req,
            grpc::ServerWriter<services::com::LogRecord>* writer) override {
        // Coarse stream-side filters (level_min / tag_filter); the fine
        // <tag-glob>:<level> DSL is client-side. The TIPC hub fan-out is
        // unfiltered.
        const uint32_t level_min = req ? req->level_min() : 0;
        const std::string tag_filter = req ? req->tag_filter() : "";

        auto sub = std::make_shared<GrpcSub>();
        {
            std::lock_guard<std::mutex> rl(g_reg_mtx);
            g_subs.insert(sub);
        }
        std::fprintf(stderr, "com: gRPC log subscriber attached "
                     "(level_min=%u tag=%s)\n", level_min,
                     tag_filter.empty() ? "*" : tag_filter.c_str());

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
            services::com::LogRecord rec;
            if (!rec.ParseFromString(raw)) continue;   // malformed — skip
            if (level_min != 0 && rec.level() < level_min) continue;
            if (!tag_filter.empty() && rec.tag() != tag_filter) continue;
            if (!writer->Write(rec)) break;            // client gone
        }
        {
            std::lock_guard<std::mutex> rl(g_reg_mtx);
            g_subs.erase(sub);
        }
        std::fprintf(stderr, "com: gRPC log subscriber detached\n");
        return grpc::Status::OK;
    }
};

std::unique_ptr<LogStreamImpl> g_svc;
std::unique_ptr<grpc::Server>  g_server;
std::atomic<bool>              g_up{false};

}  // namespace

// One-time setup: subscribe to the TIPC log hub, then build + start the gRPC
// LogStream server. If the hub isn't reachable yet we still serve gRPC (empty
// streams) — LogLink::start best-effort.
void LogForwarder::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    // The sink that bridges TIPC records → the gRPC subscriber fan-out. Set it
    // BEFORE start() so the backlog spill isn't dropped.
    services_com::LogLink::instance().set_sink(
        [](const std::string& record_bytes) { fanout(record_bytes); });

    if (!services_com::LogLink::instance().start()) {
        std::fprintf(stderr,
            "[%s] WARN: log hub (LogDaemon) not reachable; log streams will be "
            "empty until it connects\n", kNodeName);
    }

    g_svc = std::make_unique<LogStreamImpl>();
    const std::string listen = log_listen_addr();
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, services_com::make_server_creds(kNodeName));
    b.RegisterService(g_svc.get());
    g_server = b.BuildAndStart();
    if (!g_server) {
        std::fprintf(stderr, "[%s] gRPC log server failed to start on %s\n",
                     kNodeName, listen.c_str());
        g_svc.reset();
        services_com::LogLink::instance().stop();
        return;
    }
    std::fprintf(stderr, "[%s] gRPC LogStream listening on %s\n",
                 kNodeName, listen.c_str());
    g_up.store(true);
}

void LogForwarder::do_loop() {
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

void LogForwarder::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    if (g_server) {
        g_server->Shutdown(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(500));
        g_server.reset();
    }
    g_svc.reset();
    services_com::LogLink::instance().stop();
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
