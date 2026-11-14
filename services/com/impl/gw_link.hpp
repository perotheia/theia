// gw_link — com's gateway to allowlisted Theia services, RAW bytes only (proto/gateway.proto).
// No contract headers: a Call frames the request with service_id = djb2(req wire type) and
// returns the reply's bytes; a feed pg-joins the group by its wire type name and keeps a
// resume ring of the frames (gateway seq per feed). ComGrpcProxy_handlers.cc serves both over
// the gateway's own TLS gRPC server (gateway_listen).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace services_com {

struct GwFrameRec { uint64_t seq; uint64_t rx_wall_ns; std::string payload; };

class GwLink {
public:
    static GwLink& instance();
    // "RoadDomainIf=0xd0400001:0,Other=0x...:N"  and  "system_contracts_RoadReply,..."
    // Returns false on a malformed spec (nothing configured).
    bool configure(const std::string& calls, const std::string& feeds, size_t ring = 4096);
    bool start();     // pg-join every allowlisted feed (retried in the background)
    void stop();

    // status 0 OK; 3 NOT_ALLOWED (iface not allowlisted); 14 UNAVAILABLE (no answer)
    int call(const std::string& iface, const std::string& req_type, const std::string& req,
             uint32_t timeout_ms, std::string& reply, std::string& error);

    bool feed_allowed(const std::string& msg_type) const;
    // Frames with seq > since (up to `max`); blocks up to wait_ms for the first one.
    std::vector<GwFrameRec> frames_since(const std::string& msg_type, uint64_t since,
                                         int wait_ms, size_t max = 256);

private:
    GwLink();
    ~GwLink();
    struct Impl;
    Impl* impl_;
};

}  // namespace services_com
