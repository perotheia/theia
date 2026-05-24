#include "trace_decoder.hh"

#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

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

}  // namespace artheia::trace
