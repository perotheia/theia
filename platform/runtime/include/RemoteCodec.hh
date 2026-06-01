// RemoteCodec<T> + helpers: stable type IDs, nanopb encoder lookup,
// and an SFINAE-safe encode_for_trace.
//
// Extracted from NodeRef.hh so the framework's free functions in
// GenServer.hh can include the codec layer without dragging in
// LocalRef/RemoteRef/TipcClient — those depend on GenServer.hh and
// would create a circular include.

#pragma once

#include <cstdint>
#include <type_traits>

#include <pb.h>
#include <pb_encode.h>

#include "TheiaMsgHeader.hh"   // theia::runtime::TheiaMsgHeader + msg-type consts

namespace theia {
namespace runtime {

// Specialize per message type sent over the wire (or traced).
// Provides:
//   - service_id (stable 16-bit hash of the type name)
//   - fields()    (the nanopb _fields descriptor)
template <typename T>
struct RemoteCodec;

// Stable hash for service_id. djb2_low16 — matches the rule used
// elsewhere for clientServer service IDs.
constexpr uint16_t hash_msg_type_(const char* s) {
    uint32_t h = 5381;
    while (*s) { h = (h * 33) + static_cast<unsigned char>(*s++); }
    return static_cast<uint16_t>(h & 0xFFFFu);
}

// Convenience macro: emit a RemoteCodec specialization + a
// msg_type_name specialization for type T at once.
#define THEIA_DECLARE_REMOTE_CODEC(T)                               \
    namespace theia { namespace runtime {                           \
    template <>                                                     \
    struct RemoteCodec<T> {                                         \
        static constexpr uint16_t service_id = hash_msg_type_(#T);  \
        static const pb_msgdesc_t* fields() { return T##_fields; }  \
    };                                                              \
    template <> inline const char* msg_type_name<T>() { return #T; } \
    }}

// Primary `msg_type_name<T>()`. Returns "?" for types without a
// THEIA_DECLARE_REMOTE_CODEC. Defined here so GenServer.hh's trace
// hooks can call it without dragging RemoteRef/LocalRef in.
template <typename T>
const char* msg_type_name() { return "?"; }

// C++14 polyfill for std::void_t (C++17). Used by the SFINAE detector.
template <typename...> using void_t_ = void;

// Detect whether RemoteCodec<T> is specialized.
template <typename T, typename = void>
struct has_remote_codec_ : std::false_type {};
template <typename T>
struct has_remote_codec_<T, void_t_<decltype(RemoteCodec<T>::fields())>>
    : std::true_type {};

// Encode a typed message for the trace record. Returns bytes written.
//   - For types with RemoteCodec<T>: pb_encode → payload bytes.
//   - For types without: returns 0 — the trace record carries the
//     event + type tag without payload. SFINAE picks the right
//     overload at instantiation time.
template <typename T>
typename std::enable_if<has_remote_codec_<T>::value, uint16_t>::type
encode_for_trace(const T& msg, uint8_t* buf, uint16_t cap) noexcept {
    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, RemoteCodec<T>::fields(), &msg)) return 0;
    return static_cast<uint16_t>(os.bytes_written);
}

template <typename T>
typename std::enable_if<!has_remote_codec_<T>::value, uint16_t>::type
encode_for_trace(const T&, uint8_t*, uint16_t) noexcept { return 0; }

}  // namespace runtime
}  // namespace theia
