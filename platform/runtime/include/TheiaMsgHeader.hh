// theia::runtime — canonical inter-node wire header.
//
// This is the ONE on-wire framing the runtime transport (NodeRef cast/call,
// TipcMux dispatch, Tracer submit) needs: a primitive selector, a decode
// bound, and the RPC correlation triple. It owns NO bus knowledge — CAN /
// FlexRay framing, FIBEX, signal routing all belong to the gateway, not here.
//
// History: these fields used to live in libgw's gw_proto.h (GwMessageHeader),
// which forced platform/runtime to depend UP into gateway/libs/libgw — a
// generic runtime reaching into a bus-specific lib. The runtime only ever
// touched the RPC arm of that union (service_id / method_id / correlation_id);
// it never read the can/flexray arms. So the RPC subset is extracted here and
// the libgw dep is severed. The gateway keeps its own bus-superset header for
// CAN/FR framing; on the RPC wire the two layouts are byte-identical.
//
// LAYOUT IS WIRE-CONTRACT. The 24-byte packed layout below MUST match the
// peer that frames the bytes (other theia nodes; the gateway's RPC path).
// Field offsets are frozen: bus_type@0, msg_type@1, proto_len@2,
// timestamp_ns@4, rpc@12 (8B), tipc@20 (4B). Do not reorder.

#pragma once

#include <cstdint>

namespace theia {
namespace runtime {

// ── Message types ────────────────────────────────────────────────────────
// gen_server-shape inter-node messaging (per-node TIPC addressing). The TIPC
// service address identifies the destination NODE; rpc.service_id (a hash of
// the message-TYPE name) picks the right handle_cast / handle_call overload.
inline constexpr uint8_t kMsgGenCast      = 0x20u;  // node → node: async, no reply
inline constexpr uint8_t kMsgGenCall      = 0x21u;  // node → node: sync request
inline constexpr uint8_t kMsgGenCallReply = 0x22u;  // node → node: reply (corr_id)

// ── bus_type discriminator ───────────────────────────────────────────────
// The runtime only ever frames RPC. CAN / FlexRay discriminator values
// (0/1) are a gateway concern and are intentionally NOT defined here.
inline constexpr uint8_t kBusTypeRpc = 2u;

#pragma pack(push, 1)

// RPC metadata — occupies the 8-byte meta slot.
//   service_id:     djb2_low16 hash of the message-type name (RemoteCodec)
//   method_id:      0-based op index within a clientServer interface
//   correlation_id: client-assigned; server echoes verbatim. The client-side
//                   demuxer matches a reply to its outstanding future on it.
struct TheiaRpcMeta {
    uint16_t service_id;
    uint16_t method_id;
    uint32_t correlation_id;
};  // 8 bytes

// TIPC transport fields — appended after the meta slot.
//   sequence_num: monotonic per sender, for drop detection
//   reserved:     future use (priority, flags)
struct TheiaTipcMeta {
    uint16_t sequence_num;
    uint8_t  reserved[2];
};  // 4 bytes

// TheiaMsgHeader — 24 bytes, packed. Followed immediately by
// proto_data[proto_len] in the TIPC SEQPACKET datagram.
//
//   [0]     bus_type      — kBusTypeRpc for runtime traffic
//   [1]     msg_type      — kMsgGenCast | kMsgGenCall | kMsgGenCallReply
//   [2-3]   proto_len     — bytes of proto3 wire data following this header
//   [4-11]  timestamp_ns  — capture/emit timestamp (UTC ns), for the tracer
//   [12-19] rpc           — TheiaRpcMeta (8 bytes)
//   [20-23] tipc          — TheiaTipcMeta (4 bytes)
//
// The 8-byte meta slot is a bare TheiaRpcMeta (not a union): the runtime only
// frames RPC. A gateway header may overlay the same offsets with a union of
// can/flexray/rpc — the RPC arm aligns byte-for-byte with this struct.
struct TheiaMsgHeader {
    uint8_t       bus_type;
    uint8_t       msg_type;
    uint16_t      proto_len;
    uint64_t      timestamp_ns;
    TheiaRpcMeta  rpc;
    TheiaTipcMeta tipc;
};  // 1+1+2+8+8+4 = 24 bytes

#pragma pack(pop)

static_assert(sizeof(TheiaRpcMeta)   == 8,  "TheiaRpcMeta must be 8 bytes");
static_assert(sizeof(TheiaTipcMeta)  == 4,  "TheiaTipcMeta must be 4 bytes");
static_assert(sizeof(TheiaMsgHeader) == 24, "TheiaMsgHeader must be 24 bytes — wire contract");

}  // namespace runtime
}  // namespace theia
