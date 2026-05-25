// Robot node (#387) — test-only signal inject + service call into a
// running component, over the standard GwMessageHeader wire shape.
//
// com IS the robot node. A Robot test impersonates a real node and
// drives another component: this opens a TIPC client to the target's
// (type, instance), sends a GW_MSG_GEN_CAST (signal) or GW_MSG_GEN_CALL
// (service call), and for a call awaits the GW_MSG_GEN_CALL_REPLY on the
// same socket — exactly how a real node→node call routes (the receiver
// replies on the connection; we correlate by correlation_id). No
// bespoke control frame; the target's runtime TipcMux decodes it like
// any peer message.
//
// com is a libprotobuf + gRPC binary (NOT the nanopb runtime), so we
// mirror the packed 24-byte GwMessageHeader by hand — same approach the
// supervisor uses in #386. The payload bytes are built host-side (by
// the test, with the standard Python protobuf lib) and forwarded
// verbatim; the wire format is identical to nanopb on-target.
//
// Compiled only when THEIA_ROBOT_NODE is defined (test builds); a
// release build excludes this header's users entirely.

#pragma once

#include <cstdint>
#include <string>

namespace services_com {

struct RobotCallResult {
    bool        ok = false;     // false on connect/send/timeout failure
    std::string error;          // detail when !ok
    std::string reply_payload;  // GW_MSG_GEN_CALL_REPLY proto bytes
};

// djb2_low16 — MUST match platform/runtime/RemoteCodec.hh hash_msg_type_
// so the service_id we stamp equals what the target's register_cast /
// register_call computed for the nanopb C type name.
uint16_t robot_djb2_low16(const char* s) noexcept;

// Fire-and-forget GW_MSG_GEN_CAST to a TIPC name. service_id =
// robot_djb2_low16(msg_type). Returns true if the frame was sent
// (no component-level confirmation). One-shot connect/send/close.
bool robot_inject_signal(uint32_t tipc_type, uint32_t tipc_instance,
                         const std::string& msg_type,
                         const std::string& payload) noexcept;

// GW_MSG_GEN_CALL to a TIPC name; blocks up to timeout_ms for the
// matching GW_MSG_GEN_CALL_REPLY (by correlation_id) on the same
// connection, then returns its payload. service_id =
// robot_djb2_low16(req_msg_type).
RobotCallResult robot_call_service(uint32_t tipc_type, uint32_t tipc_instance,
                                   const std::string& req_msg_type,
                                   const std::string& payload,
                                   int timeout_ms) noexcept;

}  // namespace services_com
