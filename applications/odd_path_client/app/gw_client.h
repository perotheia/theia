#pragma once
#include "gw_proto.h"
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

class GwClient {
public:
    GwClient() = default;
    ~GwClient() { disconnect(); }

    bool connect();
    void disconnect();
    bool is_connected() const { return fd_ >= 0; }

    /* Block until a SIGNAL_UPDATE arrives (or timeout).
     * Returns proto_len on success, 0 on timeout, -1 on error/disconnect.
     *
     * RPC responses are intercepted internally: they never surface here.
     * The matching std::future returned by send_request() is fulfilled
     * instead. The same rx thread that drives recv_signal() also fulfills
     * RPC promises — so callers must NOT .get() the future from inside
     * that thread (e.g. from on_<port> handlers). Run RPCs from another
     * thread or from a lifecycle hook (OnStart) before the rx thread
     * spawns. */
    ssize_t recv_signal(GwMessageHeader* hdr_out,
                        uint8_t* proto_buf, size_t proto_buf_size,
                        int timeout_ms = 1000);

    /* Send a TX_REQUEST to the GW */
    bool send_tx_request(uint32_t can_id, uint8_t channel_idx, uint8_t bus_type,
                         const uint8_t* proto_data, uint16_t proto_len);

    /* RPC: send an RPC_REQUEST to the GW and return a future for the
     * RESPONSE bytes. Future is fulfilled by recv_signal() (which now
     * also runs the response demuxer) or, if no rx thread is active,
     * by pump_response() — see below. Typed proxies wrap pb_decode
     * around the returned bytes. */
    std::future<std::vector<uint8_t>> send_request(uint16_t service_id,
                                                    uint16_t method_id,
                                                    const uint8_t* req_data,
                                                    uint16_t req_len);

    /* Pump the socket once for the duration of timeout_ms looking for
     * RPC RESPONSE frames; signal frames seen in the meantime are dropped.
     * Used during OnStart (before the main rx thread runs) to do
     * synchronous RPCs that don't deadlock against recv_signal.
     * Returns true if at least one response was demuxed. */
    bool pump_response(int timeout_ms);

    /* Typed RPC: encode Req with `req_fields`, send the request, return a
     * future that — when .get()'d — decodes the response bytes via
     * `resp_fields` and yields a Resp by value. The field descriptors are
     * passed in rather than derived from the type so we don't need a
     * trait specialization per nanopb message (mirrors Google protobuf's
     * approach: typed at the boundary, descriptor at the wire). */
    template <typename Req, typename Resp>
    std::future<Resp> async_send_request(uint16_t service_id,
                                          uint16_t method_id,
                                          const Req& req,
                                          const pb_msgdesc_t* req_fields,
                                          const pb_msgdesc_t* resp_fields) {
        uint8_t      req_buf[256];
        pb_ostream_t os = pb_ostream_from_buffer(req_buf, sizeof(req_buf));
        uint16_t     req_len = 0;
        if (pb_encode(&os, req_fields, &req)) {
            req_len = static_cast<uint16_t>(os.bytes_written);
        }
        auto raw = send_request(service_id, method_id, req_buf, req_len);

        // Deferred: decode happens on the .get() thread, not on a worker.
        return std::async(std::launch::deferred,
            [raw = std::move(raw), resp_fields]() mutable {
                std::vector<uint8_t> bytes = raw.get();
                Resp resp{};
                pb_istream_t is = pb_istream_from_buffer(bytes.data(),
                                                         bytes.size());
                (void)pb_decode(&is, resp_fields, &resp);
                return resp;
            });
    }

private:
    int fd_ = -1;
    uint32_t next_correlation_id_ = 1;
    std::mutex pending_mu_;
    std::unordered_map<uint32_t,
                       std::promise<std::vector<uint8_t>>> pending_;

    /* Called from recv_signal() / pump_response() when a RESPONSE frame
     * arrives. Fulfills the matching promise; returns true if a matching
     * promise was found. */
    bool fulfill_response(const GwMessageHeader& hdr,
                          const uint8_t* proto, uint16_t proto_len);
};
