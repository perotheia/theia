#include "trace_decoder.hh"

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace artheia::trace {

void Decoder::register_msg(std::string_view msg_type_name,
                           const google::protobuf::Message* prototype) {
    prototypes_[std::string(msg_type_name)] = prototype;
}

DecodeResult Decoder::decode(std::string_view msg_type_name,
                             const uint8_t* payload,
                             std::size_t    len) const {
    auto it = prototypes_.find(msg_type_name);
    if (it == prototypes_.end()) {
        return {false, "", "no prototype for message type"};
    }
    std::unique_ptr<google::protobuf::Message> msg(it->second->New());
    if (!msg->ParseFromArray(payload, static_cast<int>(len))) {
        return {false, "", "ParseFromArray failed"};
    }
    std::string json;
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = false;
    opts.always_print_primitive_fields = true;
    auto status =
        google::protobuf::util::MessageToJsonString(*msg, &json, opts);
    if (!status.ok()) {
        return {false, "", std::string(status.ToString())};
    }
    return {true, std::move(json), ""};
}

// ---------------------------------------------------------------------------
// C ABI backing — uses a process-global Decoder populated by the
// trace_decoder_protos.cc shim's static init.
// ---------------------------------------------------------------------------

namespace {
std::mutex& globals_mu() {
    static std::mutex m;
    return m;
}
Decoder& global_decoder() {
    static Decoder d;
    return d;
}
}  // namespace

void register_global(std::string_view msg_type_name,
                     const google::protobuf::Message* prototype) {
    std::lock_guard<std::mutex> lk(globals_mu());
    global_decoder().register_msg(msg_type_name, prototype);
}

}  // namespace artheia::trace


extern "C" {

int trace_decode(const char*    msg_type_name,
                 const uint8_t* payload,
                 unsigned long  payload_len,
                 char*          out_json,
                 unsigned long  out_cap) {
    if (out_json == nullptr || out_cap == 0) return 0;
    if (msg_type_name == nullptr) {
        std::snprintf(out_json, out_cap, "null msg_type_name");
        return 0;
    }
    artheia::trace::DecodeResult r;
    {
        std::lock_guard<std::mutex> lk(artheia::trace::globals_mu());
        r = artheia::trace::global_decoder().decode(
            msg_type_name, payload, static_cast<std::size_t>(payload_len));
    }
    if (!r.ok) {
        std::snprintf(out_json, out_cap, "%s", r.error.c_str());
        return 0;
    }
    if (r.json.size() + 1 > out_cap) {
        std::snprintf(out_json, out_cap, "output buffer too small");
        return 0;
    }
    std::memcpy(out_json, r.json.data(), r.json.size());
    out_json[r.json.size()] = '\0';
    return static_cast<int>(r.json.size());
}

unsigned long trace_decoder_size(void) {
    std::lock_guard<std::mutex> lk(artheia::trace::globals_mu());
    return static_cast<unsigned long>(
        artheia::trace::global_decoder().size());
}

}  // extern "C"
