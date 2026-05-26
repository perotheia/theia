// services/log[trace] — TraceCollector daemon + its OWN gRPC TraceStream.
//
// Egress-direct design (docs/tasks/BACKLOG/trace-to-rf-via-com.md): every
// reporting FC submits trace records over TIPC GW_MSG_GEN_CAST to this
// collector's in_records port (0x80010013). The collector serves them out
// over its OWN gRPC TraceStream.Subscribe — com is NOT in the byte path
// (com only governs the DMZ + owns the control path, SupervisorView.
// ConfigureTrace).
//
// Data path (all in ONE process — no second TIPC hop):
//   FC --TIPC GEN_CAST(TraceRecord)--> [TIPC receiver thread]
//        --> TraceHub (in-proc subscriber registry)
//        --> gRPC Subscribe streams (src/dst rewritten addr->name)
//
// Wire note: the record on the bus is proto3-wire TraceRecord (the
// runtime hand-encodes it, Tracer.hh) — so this server parses it straight
// into the libprotobuf services::com::TraceRecord; NO nanopb here.

#include "supervisor_bridge.grpc.pb.h"   // services::com::TraceRecord, TraceSubscribeRequest
#include "trace_stream.grpc.pb.h"        // services::log::TraceStream

#include <nlohmann/json.hpp>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <csignal>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// TraceCollector's in_records TIPC name — services/log/system/package.art.
constexpr uint32_t kCollectorTipcType     = 0x80010013u;
constexpr uint32_t kCollectorTipcInstance = 0u;

// GwMessageHeader is 24 bytes; the trace submit rides GW_MSG_GEN_CAST with
// the proto3-wire TraceRecord as the payload after the header. We only
// need to skip the header and parse the payload — no libgw dependency.
constexpr size_t kGwHeaderLen = 24;

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }

// ---- TraceHub: in-process record fanout to gRPC subscribers ------------
//
// The TIPC receiver thread push()es decoded records; each gRPC Subscribe
// call holds a Sub and drains its queue. Bounded queue — a slow/abandoned
// subscriber drops oldest (trace is best-effort, never block ingest).
struct Sub {
    std::mutex                              mtx;
    std::condition_variable                 cv;
    std::deque<services::com::TraceRecord>  queue;
    bool                                    closed = false;
    uint32_t                                kind_filter = 0;     // 0 = all
    std::string                             node_filter;         // "" = all
    static constexpr size_t kMaxQueue = 4096;
};

class TraceHub {
public:
    std::shared_ptr<Sub> subscribe(uint32_t kind, std::string node) {
        auto s = std::make_shared<Sub>();
        s->kind_filter = kind;
        s->node_filter = std::move(node);
        std::lock_guard<std::mutex> lk(mtx_);
        subs_.push_back(s);
        return s;
    }
    void unsubscribe(const std::shared_ptr<Sub>& s) {
        if (!s) return;
        { std::lock_guard<std::mutex> lk(s->mtx); s->closed = true; }
        s->cv.notify_all();
        std::lock_guard<std::mutex> lk(mtx_);
        subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                    [&](const std::weak_ptr<Sub>& w) {
                        auto sp = w.lock();
                        return !sp || sp == s;
                    }), subs_.end());
    }
    // Fan one record to every matching subscriber (filters applied here so
    // the gRPC thread doesn't re-check). Record is already addr->name
    // rewritten by the caller.
    void publish(const services::com::TraceRecord& rec) {
        std::vector<std::shared_ptr<Sub>> snap;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& w : subs_) if (auto sp = w.lock()) snap.push_back(sp);
        }
        for (auto& s : snap) {
            if (s->kind_filter && rec.kind() != s->kind_filter) continue;
            if (!s->node_filter.empty() && rec.node_name() != s->node_filter)
                continue;
            {
                std::lock_guard<std::mutex> lk(s->mtx);
                if (s->closed) continue;
                if (s->queue.size() >= Sub::kMaxQueue) s->queue.pop_front();
                s->queue.push_back(rec);
            }
            s->cv.notify_one();
        }
    }
private:
    std::mutex                          mtx_;
    std::vector<std::weak_ptr<Sub>>     subs_;
};

// ---- Netgraph digest: TIPC (type,instance) -> component name -----------
//
// Loaded once at startup from the cluster netgraph.json (artheia
// gen-netgraph -R). On egress the collector rewrites src/dst from the
// node's TIPC address to its component name. Missing file / unknown addr
// degrade gracefully: the address (or original value) is left as-is.
class NetgraphMap {
public:
    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "[trace_collector] no netgraph at %s — "
                         "src/dst stay as-is (no addr->name rewrite)\n",
                         path.c_str());
            return;
        }
        try {
            nlohmann::json j; f >> j;
            for (auto& n : j.at("nodes")) {
                if (!n.contains("name") || !n.contains("tipc")) continue;
                const auto& t = n["tipc"];
                if (!t.contains("type")) continue;
                uint64_t type = std::stoull(t["type"].get<std::string>(),
                                            nullptr, 0);
                by_type_[static_cast<uint32_t>(type)] =
                    n["name"].get<std::string>();
            }
            std::fprintf(stderr, "[trace_collector] netgraph: %zu node "
                         "address->name mappings\n", by_type_.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[trace_collector] netgraph parse failed: "
                         "%s — src/dst stay as-is\n", e.what());
        }
    }
    // Resolve a "0x...." hex address string to a component name, or return
    // the input unchanged if unknown / not an address.
    std::string resolve(const std::string& v) const {
        if (v.empty() || by_type_.empty()) return v;
        // The producer puts the node's kNodeName (snake) in node_name, and
        // dst is "" today (no peer). Once src/dst carry addresses, this
        // maps them; for now it's a no-op passthrough for names.
        try {
            uint64_t a = std::stoull(v, nullptr, 0);
            auto it = by_type_.find(static_cast<uint32_t>(a));
            if (it != by_type_.end()) return it->second;
        } catch (...) { /* not a number → leave as-is */ }
        return v;
    }
private:
    std::unordered_map<uint32_t, std::string> by_type_;
};

// ---- gRPC TraceStream.Subscribe ----------------------------------------
class TraceStreamImpl final : public services::log::TraceStream::Service {
public:
    explicit TraceStreamImpl(TraceHub* hub) : hub_(hub) {}

    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::log::TraceSubscribeRequest* req,
            grpc::ServerWriter<services::com::TraceRecord>* writer) override {
        auto sub = hub_->subscribe(req->kind(), req->target_node());
        std::fprintf(stderr, "[trace_collector] gRPC subscriber attached "
                     "(kind=%u node='%s')\n", req->kind(),
                     req->target_node().c_str());
        while (!ctx->IsCancelled() && g_running.load()) {
            services::com::TraceRecord rec;
            {
                std::unique_lock<std::mutex> lk(sub->mtx);
                sub->cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                    return sub->closed || !sub->queue.empty();
                });
                if (sub->closed && sub->queue.empty()) break;
                if (sub->queue.empty()) continue;
                rec = std::move(sub->queue.front());
                sub->queue.pop_front();
            }
            if (!writer->Write(rec)) break;   // client gone
        }
        hub_->unsubscribe(sub);
        std::fprintf(stderr, "[trace_collector] gRPC subscriber detached\n");
        return grpc::Status::OK;
    }

private:
    TraceHub* hub_;
};

// ---- TIPC in_records receiver ------------------------------------------
// Binds the collector's TIPC name and accepts GEN_CAST frames carrying a
// proto3-wire TraceRecord. Each record is rewritten (src/dst addr->name)
// and published to the hub. SOCK_DGRAM matches the producer's submit
// (Tracer.hh TraceSubmitter — lossy-OK firehose); a datagram =
// [24B GwHeader][proto3 TraceRecord].
void run_receiver(TraceHub* hub, const NetgraphMap* ng) {
    int fd = ::socket(AF_TIPC, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "[trace_collector] TIPC socket failed — "
                     "no record ingest\n");
        return;
    }
    struct sockaddr_tipc addr{};
    addr.family                  = AF_TIPC;
    addr.addrtype                = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = kCollectorTipcType;
    addr.addr.name.name.instance = kCollectorTipcInstance;
    addr.scope                   = TIPC_NODE_SCOPE;
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        std::fprintf(stderr, "[trace_collector] TIPC bind 0x%x failed — "
                     "no record ingest\n", kCollectorTipcType);
        ::close(fd);
        return;
    }
    std::fprintf(stderr, "[trace_collector] up — in_records TIPC "
                 "type=0x%x instance=%u\n",
                 kCollectorTipcType, kCollectorTipcInstance);

    std::vector<uint8_t> buf(64 * 1024);
    while (g_running.load()) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            if (!g_running.load()) break;
            continue;
        }
        if (static_cast<size_t>(n) <= kGwHeaderLen) continue;  // header-only
        services::com::TraceRecord rec;
        if (!rec.ParseFromArray(buf.data() + kGwHeaderLen,
                                static_cast<int>(n - kGwHeaderLen))) {
            continue;  // malformed — drop
        }
        // Egress rewrite: src/dst addr -> component name (no-op for names).
        rec.set_node_name(ng->resolve(rec.node_name()));
        if (!rec.dst().empty()) rec.set_dst(ng->resolve(rec.dst()));
        hub->publish(rec);
    }
    ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::string listen   = "0.0.0.0:7710";
    std::string netgraph = "netgraph.json";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen" && i + 1 < argc)        listen   = argv[++i];
        else if (a == "--netgraph" && i + 1 < argc) netgraph = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::fprintf(stderr, "usage: %s [--listen HOST:PORT] "
                         "[--netgraph PATH]\n", argv[0]);
            return 0;
        }
    }
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    NetgraphMap ng;
    ng.load(netgraph);

    TraceHub hub;
    std::thread rx(run_receiver, &hub, &ng);

    TraceStreamImpl svc(&hub);
    grpc::ServerBuilder b;
    b.AddListeningPort(listen, grpc::InsecureServerCredentials());
    b.RegisterService(&svc);
    std::unique_ptr<grpc::Server> server(b.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "[trace_collector] gRPC failed to start on %s\n",
                     listen.c_str());
        g_running.store(false);
        if (rx.joinable()) rx.join();
        return 2;
    }
    std::fprintf(stderr, "[trace_collector] gRPC TraceStream listening on %s\n",
                 listen.c_str());

    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::fprintf(stderr, "[trace_collector] shutting down\n");
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));
    if (rx.joinable()) rx.join();
    return 0;
}
