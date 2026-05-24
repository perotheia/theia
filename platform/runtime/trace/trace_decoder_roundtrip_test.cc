// Round-trip: nanopb encode → proto-wire-v3 bytes → libprotobuf decode → JSON.
//
// This is the entire premise of libtrace_decoder. The runtime
// (Tracer.hh) emits nanopb-encoded payloads; host tools must parse
// them with libprotobuf and surface JSON to humans. If the wire
// format agrees between the two stacks for one non-trivial message,
// the design is sound; adding more types is just registering more
// prototypes against the same Decoder.

#include "trace_decoder.hh"

#include <pb_encode.h>

// Both proto stacks generate a file called `sm.pb.h` — distinguish
// them by include path. The nanopb header lives at the .proto's
// directory (no `includes=[]` on its filegroup) so use the
// workspace-rooted path. The libprotobuf header is reached via
// `cpp/` on the include search path.
extern "C" {
#include "platform/proto/system/services/sm/sm.pb.h"  // nanopb
}

#include "sm.pb.h"   // libprotobuf (cpp/ is on -I via sm_pb_cpp)

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

// Encode one SmStateMsg via nanopb. Returns bytes written or -1.
int nanopb_encode_sm_state(uint8_t* out, std::size_t out_cap,
                           services_services_sm_SmState state,
                           uint64_t ts_ns) {
    services_services_sm_SmStateMsg msg = {};
    msg.state = state;
    msg.ts_ns = ts_ns;

    pb_ostream_t stream = pb_ostream_from_buffer(out, out_cap);
    if (!pb_encode(&stream, services_services_sm_SmStateMsg_fields, &msg)) {
        return -1;
    }
    return static_cast<int>(stream.bytes_written);
}

void test_roundtrip_basic() {
    uint8_t buf[64];
    int n = nanopb_encode_sm_state(
        buf, sizeof(buf),
        services_services_sm_SmState_RUNNING,
        1700000000123456789ULL);
    assert(n > 0);
    std::printf("nanopb wrote %d bytes\n", n);

    artheia::trace::Decoder dec;
    // Use the libprotobuf-generated default_instance as the prototype.
    // The C++ proto's package is "services_services_sm", matching the
    // nanopb-side `services_services_sm_SmStateMsg_fields` table — so
    // the wire-format definitions are byte-identical.
    dec.register_msg(
        "SmStateMsg",
        &services_services_sm::SmStateMsg::default_instance());

    auto result = dec.decode("SmStateMsg", buf, static_cast<std::size_t>(n));
    if (!result.ok) {
        std::fprintf(stderr, "decode failed: %s\n", result.error.c_str());
        std::abort();
    }
    std::printf("JSON: %s\n", result.json.c_str());

    // Verify the JSON contains both fields with the values we encoded.
    // Enum surfaces as its string name, ts_ns as a string (proto3 JSON
    // serializes uint64/int64 as strings to dodge JS precision loss).
    assert(result.json.find("\"state\":\"RUNNING\"") != std::string::npos);
    assert(result.json.find("\"tsNs\":\"1700000000123456789\"")
               != std::string::npos);
}

void test_unknown_type_returns_error() {
    artheia::trace::Decoder dec;
    uint8_t dummy[] = {0x08, 0x01};
    auto result = dec.decode("NoSuchType", dummy, sizeof(dummy));
    assert(!result.ok);
    assert(!result.error.empty());
    assert(result.json.empty());
}

void test_empty_payload_decodes_to_default() {
    // Empty payload is a valid proto3 message: all fields default.
    // SmStateMsg has state=OFF (=0) and ts_ns=0. With
    // always_print_primitive_fields the JSON still lists both.
    artheia::trace::Decoder dec;
    dec.register_msg(
        "SmStateMsg",
        &services_services_sm::SmStateMsg::default_instance());

    auto result = dec.decode("SmStateMsg", nullptr, 0);
    assert(result.ok);
    std::printf("empty-payload JSON: %s\n", result.json.c_str());
    assert(result.json.find("\"state\":\"OFF\"") != std::string::npos);
    assert(result.json.find("\"tsNs\":\"0\"") != std::string::npos);
}

}  // namespace

int main() {
    test_roundtrip_basic();
    test_unknown_type_returns_error();
    test_empty_payload_decodes_to_default();
    std::printf("all trace_decoder_roundtrip_test cases passed\n");
    return 0;
}
