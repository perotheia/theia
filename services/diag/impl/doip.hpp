// doip.hpp — the DoIP (ISO 13400) wire model behind the diag FC. APP-OWNED,
// in-process + header-only so it's unit-testable (like nm_backend / release_dir).
//
// DoIP frames a generic header in front of every message:
//
//   byte 0   protocol version       (0x02 = DoIP ISO 13400-2:2012)
//   byte 1   inverse protocol ver   (~version, integrity check)
//   bytes 2-3 payload type          (big-endian u16)
//   bytes 4-7 payload length        (big-endian u32)
//   bytes 8.. payload
//
// We implement the payload types the tester handshake + diagnostic exchange need:
//   0x0001 VehicleIdentificationRequest         (UDP discovery, tester→ECU)
//   0x0004 VehicleAnnouncement / IdentResponse  (UDP, ECU→tester)
//   0x0005 RoutingActivationRequest             (TCP, tester→ECU — the auth gate)
//   0x0006 RoutingActivationResponse            (TCP, ECU→tester)
//   0x8001 DiagnosticMessage                    (TCP, carries the UDS payload)
//   0x8002 DiagnosticMessage positive ACK
//   0x8003 DiagnosticMessage negative ACK
//
// Everything is best-effort + bounds-checked: a short/malformed buffer parses to
// `false`, never reads out of range.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ara::diag {

namespace doip {

constexpr uint8_t  kVersion = 0x02;                 // ISO 13400-2:2012
constexpr size_t   kHeaderLen = 8;

// Payload types (ISO 13400-2 Table 17).
enum PayloadType : uint16_t {
    PT_VehicleIdReq       = 0x0001,
    PT_VehicleAnnounce    = 0x0004,
    PT_RoutingActivReq    = 0x0005,
    PT_RoutingActivResp   = 0x0006,
    PT_AliveCheckReq      = 0x0007,
    PT_DiagMessage        = 0x8001,
    PT_DiagMessageAck     = 0x8002,
    PT_DiagMessageNack    = 0x8003,
};

// Routing-activation response codes (ISO 13400-2 Table 25). 0x10 = success.
enum RoutingActivCode : uint8_t {
    RA_Success            = 0x10,
    RA_UnknownSource      = 0x00,
    RA_NoResources        = 0x02,
};

using Bytes = std::vector<uint8_t>;

inline void put_u16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}
inline void put_u32(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));  b.push_back(uint8_t(v));
}
inline uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
           uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

// A parsed DoIP message: the payload type + the payload bytes (header stripped).
struct Message {
    uint16_t type = 0;
    Bytes    payload;
};

// Frame a DoIP message: header (version + inverse + type + length) + payload.
inline Bytes frame(uint16_t type, const Bytes& payload) {
    Bytes b;
    b.push_back(kVersion);
    b.push_back(uint8_t(~kVersion));
    put_u16(b, type);
    put_u32(b, uint32_t(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

// Parse ONE DoIP message from `buf`. On success fills `out` + sets `consumed` to
// the total bytes (header+payload) used, returns true. Returns false on a short
// buffer (need more bytes) or a bad version. `need_more` distinguishes "wait for
// more TCP bytes" from "malformed".
inline bool parse(const uint8_t* buf, size_t len, Message& out,
                  size_t& consumed, bool& need_more) {
    need_more = false;
    if (len < kHeaderLen) { need_more = true; return false; }
    if (buf[0] != kVersion || buf[1] != uint8_t(~kVersion)) return false; // bad
    uint16_t type = get_u16(buf + 2);
    uint32_t plen = get_u32(buf + 4);
    if (len < kHeaderLen + plen) { need_more = true; return false; }       // partial
    out.type = type;
    out.payload.assign(buf + kHeaderLen, buf + kHeaderLen + plen);
    consumed = kHeaderLen + plen;
    return true;
}

// ── Routing activation ──────────────────────────────────────────────────────
// Request payload (ISO 13400-2 Table 24): source address (u16) + activation type
// (u8) + reserved (u32) [+ optional OEM u32]. We only need the source address.
struct RoutingActivReq {
    uint16_t source_addr = 0;
    uint8_t  activation_type = 0;
};
inline bool parse_routing_activ(const Bytes& p, RoutingActivReq& r) {
    if (p.size() < 7) return false;        // src(2)+type(1)+reserved(4)
    r.source_addr = get_u16(p.data());
    r.activation_type = p[2];
    return true;
}
// Response payload (Table 25): tester addr (u16) + ECU addr (u16) + code (u8) +
// reserved (u32).
inline Bytes make_routing_activ_resp(uint16_t tester, uint16_t ecu,
                                     uint8_t code) {
    Bytes p;
    put_u16(p, tester);
    put_u16(p, ecu);
    p.push_back(code);
    put_u32(p, 0);                          // reserved
    return frame(PT_RoutingActivResp, p);
}

// ── Diagnostic message ──────────────────────────────────────────────────────
// Payload (ISO 13400-2 Table 28): source addr (u16) + target addr (u16) + the
// UDS data. Carries the actual UDS request/response.
struct DiagMessage {
    uint16_t source_addr = 0;
    uint16_t target_addr = 0;
    Bytes    uds;
};
inline bool parse_diag_message(const Bytes& p, DiagMessage& m) {
    if (p.size() < 4) return false;
    m.source_addr = get_u16(p.data());
    m.target_addr = get_u16(p.data() + 2);
    m.uds.assign(p.begin() + 4, p.end());
    return true;
}
inline Bytes make_diag_message(uint16_t source, uint16_t target,
                               const Bytes& uds) {
    Bytes p;
    put_u16(p, source);
    put_u16(p, target);
    p.insert(p.end(), uds.begin(), uds.end());
    return frame(PT_DiagMessage, p);
}
// Positive ACK (Table 30): source+target+ack code(0x00)[+ echoed message].
inline Bytes make_diag_ack(uint16_t source, uint16_t target) {
    Bytes p;
    put_u16(p, source);
    put_u16(p, target);
    p.push_back(0x00);                       // ack code = received
    return frame(PT_DiagMessageAck, p);
}

// ── Vehicle identification / announcement (UDP discovery) ───────────────────
// Announcement payload (Table 21): VIN(17) + logical addr(2) + EID(6) + GID(6) +
// further-action(1). We fill a minimal valid response.
inline Bytes make_vehicle_announcement(uint16_t ecu_addr,
                                       const std::string& vin) {
    Bytes p;
    std::string v = vin;
    v.resize(17, '0');                       // VIN is exactly 17 bytes
    p.insert(p.end(), v.begin(), v.end());
    put_u16(p, ecu_addr);
    for (int i = 0; i < 6; ++i) p.push_back(0);  // EID
    for (int i = 0; i < 6; ++i) p.push_back(0);  // GID
    p.push_back(0x00);                       // further action required = none
    return frame(PT_VehicleAnnounce, p);
}

}  // namespace doip
}  // namespace ara::diag
