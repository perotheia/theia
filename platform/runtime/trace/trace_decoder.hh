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

}  // namespace artheia::trace
