// SupFirehose — reassembles the supervisor's #429 topo-pair stream into a
// libprotobuf TreeSnapshot and fans it out to the gRPC Subscribe streams.
//
// This RETIRES services/com/impl/tipc_uplink: the supervisor no longer
// publishes a monolithic TreeSnapshot on a bespoke socket. Instead it CASTs a
// stream of small fixed nanopb messages over the STANDARD Theia transport
// (GwMessageHeader + nanopb) to com's ComDaemon (TIPC 0x80010008/0). main.cc
// register_casts the four service_ids and feeds the decoded values here.
//
// SupFirehose's public API takes PLAIN SCALARS / strings — no proto type
// crosses its boundary. The caller (main.cc's dispatch lambda) decodes the
// NANOPB structs; SupFirehose.cc builds the LIBPROTOBUF TreeSnapshot
// internally. The two proto worlds (same-basename .pb.h) therefore never meet
// in one TU. On SnapshotEnd it serialises the rebuilt TreeSnapshot and pushes
// a Frame{kTagSnapshot, bytes} to every subscriber — the SAME shape the gRPC
// Subscribe handler already consumes, so the gRPC face (SupervisorObservation
// oneof) and every client (supervisor-gui / supdbg / rf-theia) are UNCHANGED.
//
// Threading: the cast dispatch runs on the ComDaemon config_mux epoll thread;
// the gRPC Subscribe handlers run on grpc++ worker threads. The reassembly
// buffer + the subscriber list are mutex-guarded.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace services_com {

// One inbound observation, opaque to subscribers — they decode the protobuf
// themselves by tag. Mirrors the old TipcUplink::Frame so the gRPC Subscribe
// handler is unchanged.
struct Frame {
    uint16_t    tag;
    std::string payload;       // protobuf bytes (a serialized TreeSnapshot)
};

// Subscriber sink — owned by the gRPC stream handler.
struct Subscriber {
    std::mutex                          mtx;
    std::condition_variable             cv;
    std::deque<Frame>                   queue;
    bool                                closed = false;
};

// Frame tags — shared with the supervisor + the gRPC Subscribe handler.
constexpr uint16_t kTagEvent    = 0x0001;
constexpr uint16_t kTagHealth   = 0x0002;
constexpr uint16_t kTagSnapshot = 0x0003;

class SupFirehose {
public:
    static SupFirehose& instance();

    // ---- gRPC subscriber fan-out (same surface TipcUplink exposed) -------
    std::shared_ptr<Subscriber> subscribe();
    void unsubscribe(const std::shared_ptr<Subscriber>& s);

    // ---- stream sinks (called from the cast dispatch, mux thread) --------
    // Plain-scalar API: no proto types cross this boundary.
    void on_snapshot_begin(uint64_t generation, uint64_t timestamp_ms);
    void on_node_edge(uint32_t op, const std::string& parent_name,
                      const std::string& name, uint32_t kind);
    void on_node_state(const std::string& name, int32_t pid, uint32_t tid,
                       uint32_t state, uint32_t flags, uint32_t restart_count,
                       int32_t last_exit_code, uint64_t uptime_ms,
                       uint32_t cpu_pct, uint64_t rss_kb, uint64_t vsz_kb,
                       uint32_t threads, uint64_t shared_kb, uint64_t data_kb);
    void on_snapshot_end(uint64_t generation);

private:
    SupFirehose() = default;

    void push_frame_(uint16_t tag, std::string payload);

    // Reassembly buffer (impl detail; defined in the .cc which owns the
    // libprotobuf TreeSnapshot). Held by opaque pointer so this header needs
    // no proto include.
    struct Build;
    std::mutex                              build_mtx_;
    std::unique_ptr<Build>                  build_;
    uint64_t                                open_generation_ = 0;
    bool                                    in_snapshot_ = false;

    std::mutex                              sub_mtx_;
    std::vector<std::weak_ptr<Subscriber>>  subs_;
};

}  // namespace services_com
