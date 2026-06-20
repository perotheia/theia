#pragma once

// libtrace_decoder — host-side decoder for binary trace records.
//
// Runtime emits records as `[header text][hex payload]`, where the
// payload is the proto-wire-v3 bytes of one nanopb-encoded message
// (e.g. `SmStateMsg`). The header carries the message-type name
// already (see Tracer.hh emit() format), so the decoder doesn't
// have to guess the type at runtime — it looks up the name in a
// registry built from cc_proto_library-compiled message factories.
//
// Output is JSON via google::protobuf::util::MessageToJsonString,
// which is what supdbg + supervisor-gui surface to humans.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace google::protobuf { class Message; }

namespace artheia::trace {

struct DecodeResult {
    bool        ok;
    std::string json;    // populated iff ok
    std::string error;   // populated iff !ok
};

class Decoder {
 public:
    // Register a prototype Message for `msg_type_name`. The decoder
    // does NOT take ownership: the prototype is typically the
    // generated `*::default_instance()`. Lifetime must outlive the
    // decoder. Re-registering the same name overwrites silently.
    void register_msg(std::string_view msg_type_name,
                      const google::protobuf::Message* prototype);

    // Decode `payload` against the registered prototype for
    // `msg_type_name`. On success, JSON lands in result.json.
    DecodeResult decode(std::string_view msg_type_name,
                        const uint8_t* payload,
                        std::size_t    len) const;

    std::size_t size() const noexcept { return prototypes_.size(); }

 private:
    // std::less<> enables transparent string_view lookups against
    // std::string keys without an allocation.
    std::map<std::string, const google::protobuf::Message*, std::less<>>
        prototypes_;
};

// Register a prototype on the process-global Decoder that backs the
// C ABI (`trace_decode()`). The trace_decoder_protos.cc shim calls
// this at static-init for every proto type the .so was linked with.
void register_global(std::string_view msg_type_name,
                     const google::protobuf::Message* prototype);

}  // namespace artheia::trace


// ---------------------------------------------------------------------------
// C ABI — for ctypes consumers (rf-theia adapter, supdbg Python plugin).
//
// Wraps a process-global Decoder pre-populated by static init with every
// message type the library was built against. Callers don't manage
// prototypes; they only call `trace_decode()`.
// ---------------------------------------------------------------------------

extern "C" {

// Decode `payload` (raw proto-wire-v3 bytes) against the message type
// named `msg_type_name`. JSON written to `out_json` (NUL-terminated)
// up to `out_cap-1` bytes.
//
// Returns:
//   >0 = JSON length in bytes (excluding NUL)
//    0 = unknown type / parse failure / output too small. On 0 return,
//        `out_json` carries a NUL-terminated error message.
int trace_decode(const char*    msg_type_name,
                 const uint8_t* payload,
                 unsigned long  payload_len,
                 char*          out_json,
                 unsigned long  out_cap);

// Convenience accessor: how many message types are registered. Useful
// for sanity-checking the library was built with the protos you
// expected.
unsigned long trace_decoder_size(void);

// Release/version string for THIS plugin .so. Each libtrace_decoder_*.so
// (framework `libtrace_decoder_system.so`, app `libtrace_decoder_apps.so`,
// …) exports its own. The pluggable-decoder loader reads every plugin's
// value and, when an app plugin's version disagrees with the framework
// system plugin's, logs a WARNING (it does NOT hard-fail) — a wire-format
// drift early-warning. Defined in each plugin's *_protos.cc registrar.
const char* trace_decoder_release_ver(void);

}  // extern "C"
