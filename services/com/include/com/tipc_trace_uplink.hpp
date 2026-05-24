// services/com — TIPC client for the log[trace] FC's fanout port.
//
// Companion to TipcUplink (which talks to platform/supervisor): this
// one talks to services/log[trace] (TIPC type 0x80010013 — the
// TraceCollector node). The wire format is plain framed protobuf:
//
//   [u16 tag][u32 len][bytes payload]
//
// where tag distinguishes TraceRecord (inbound stream) from
// ApplyConfig (outbound — supdbg toggling a (node, msg_type) filter).
//
// Same Subscriber/Frame pattern as TipcUplink so the gRPC
// TraceStreamImpl can mirror SupervisorViewImpl's structure.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace services_com {

struct TraceFrame {
    uint16_t    tag;
    std::string payload;  // protobuf-serialised TraceRecord
};

struct TraceSubscriber {
    std::mutex                          mtx;
    std::condition_variable             cv;
    std::deque<TraceFrame>              queue;
    bool                                closed = false;
};

class TipcTraceUplink {
public:
    // Defaults match the TraceCollector node declared in
    // services/log/lib/TraceCollector.hh.
    explicit TipcTraceUplink(uint32_t trace_tipc_type     = 0x80010013,
                             uint32_t trace_tipc_instance = 0);
    ~TipcTraceUplink();

    TipcTraceUplink(const TipcTraceUplink&)            = delete;
    TipcTraceUplink& operator=(const TipcTraceUplink&) = delete;

    // Open the TIPC connection + start the reader thread. Returns
    // true on success; on failure the uplink is in a stopped state
    // and the caller should still construct an empty service (the
    // gRPC server can run even if no trace node is up — subscribers
    // will just see no records).
    bool start();
    void stop();
    bool running() const noexcept { return running_.load(); }

    // Subscribe to inbound trace frames. Returns a shared_ptr the
    // gRPC stream handler waits on; the uplink keeps a weak_ptr so
    // it can detect when the stream is gone and drop the slot.
    std::shared_ptr<TraceSubscriber> subscribe();
    void unsubscribe(const std::shared_ptr<TraceSubscriber>& s);

    // Submit a TraceConfigRequest (target_node, msg_type, enabled)
    // to the log[trace] FC. The supervisor's NodeTraceCtl path
    // (#361) takes it from there. Returns true on send success —
    // there is no reply for this call today (fire-and-forget per
    // the dbg-shaped spec).
    bool send_config_request(const std::string& serialized_request);

private:
    void run();

    uint32_t  type_;
    uint32_t  instance_;
    int       fd_      = -1;
    std::atomic<bool>  running_{false};
    std::thread thread_;

    std::mutex                                       sub_mtx_;
    std::vector<std::weak_ptr<TraceSubscriber>>      subs_;
};

}  // namespace services_com
