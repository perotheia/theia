// TIPC uplink — connects the com bridge to the SUPERVISOR's TIPC service.
//
// NOTE: this is NOT a services/log artifact. It belongs to the World-A
// gRPC bridge (services-com, src/main.cpp): the bridge's single persistent
// uplink to the supervisor. It is load-bearing for SupervisorView.Subscribe
// (fan-out of observation frames to gRPC subscribers) and the unary
// mutators (ControlRequest → supervisor, ControlReply back). The generic
// Frame{tag,payload} naming is bridge-internal, not a log shape.
//
// One persistent SOCK_SEQPACKET socket to (type=0x80020001, instance=0).
// A reader thread fans inbound frames to every subscribed gRPC stream
// via a per-subscriber lock-free SPSC queue. ControlRequest frames go
// out the same socket; replies pair up by correlation_id via a
// pending-reply map.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace services_com {

// One inbound observation, opaque to subscribers — they decode the
// protobuf themselves based on the tag.
struct Frame {
    uint16_t    tag;
    std::string payload;       // protobuf bytes
};

// Subscriber sink — owned by the gRPC stream handler; the uplink
// pushes to it under sub_mtx_.
struct Subscriber {
    std::mutex                          mtx;
    std::condition_variable             cv;
    std::deque<Frame>                   queue;
    bool                                closed = false;
};

class TipcUplink {
public:
    explicit TipcUplink(uint32_t sup_tipc_type     = 0x80020001,
                        uint32_t sup_tipc_instance = 0);
    ~TipcUplink();

    TipcUplink(const TipcUplink&)            = delete;
    TipcUplink& operator=(const TipcUplink&) = delete;

    // Open the connection + start the reader thread.
    bool start();
    void stop();

    // Subscribe to inbound observations. Returns a shared_ptr the
    // gRPC stream handler waits on; the uplink keeps a weak_ptr so it
    // can detect when the stream is gone and stop pushing.
    std::shared_ptr<Subscriber> subscribe();
    void unsubscribe(const std::shared_ptr<Subscriber>& s);

    // Submit a ControlRequest payload. Blocks (up to timeout_ms) for
    // the ControlReply matching the embedded correlation_id; returns
    // false on timeout. The serialised payload should be a fully-formed
    // ControlRequest protobuf (services.supervisor.ControlRequest); the
    // uplink frames it with the 2-byte tag itself.
    bool send_control_request(const std::string& serialized_request,
                               uint64_t correlation_id,
                               std::string& out_reply_payload,
                               int timeout_ms = 5000);

    uint64_t next_correlation_id() { return ++correlation_seq_; }

private:
    void run();

    uint32_t  type_;
    uint32_t  instance_;
    int       fd_      = -1;
    std::atomic<bool>  running_{false};
    std::thread thread_;

    std::mutex                                 sub_mtx_;
    std::vector<std::weak_ptr<Subscriber>>     subs_;

    // Pending reply state.
    struct Pending {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    have_reply = false;
        std::string             reply_payload;
    };
    std::mutex                                            pending_mtx_;
    std::map<uint64_t, std::shared_ptr<Pending>>          pending_;
    std::atomic<uint64_t>                                  correlation_seq_{0};
};

}  // namespace services_com
