#pragma once
#include "gw_proto.h"
#include <cstdint>
#include <string>
#include <sys/types.h>

class GwClient {
public:
    GwClient() = default;
    ~GwClient() { disconnect(); }

    bool connect();
    void disconnect();
    bool is_connected() const { return fd_ >= 0; }

    /* Block until a SIGNAL_UPDATE arrives (or timeout).
     * Returns proto_len on success, 0 on timeout, -1 on error/disconnect. */
    ssize_t recv_signal(GwMessageHeader* hdr_out,
                        uint8_t* proto_buf, size_t proto_buf_size,
                        int timeout_ms = 1000);

    /* Send a TX_REQUEST to the GW */
    bool send_tx_request(uint32_t can_id, uint8_t channel_idx, uint8_t bus_type,
                         const uint8_t* proto_data, uint16_t proto_len);

private:
    int fd_ = -1;
};
