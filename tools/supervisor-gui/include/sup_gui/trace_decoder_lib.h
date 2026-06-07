// trace_decoder_lib — dlopen wrapper around libtrace_decoder_ctypes.so.
// Decodes a trace record's proto payload to JSON for the trace panel's
// detail pane. See trace_decoder_lib.cpp for the .so discovery rules.

#pragma once

#include <memory>
#include <string>

namespace sup_gui {

class TraceDecoderLib {
public:
    TraceDecoderLib();
    ~TraceDecoderLib();
    TraceDecoderLib(const TraceDecoderLib&)            = delete;
    TraceDecoderLib& operator=(const TraceDecoderLib&) = delete;

    // True if libtrace_decoder_ctypes.so was found + opened.
    bool available() const;

    // Decode `payload` (raw proto-wire-v3 bytes) against `msg_type` (the wire
    // name the record carries, e.g. "system_demo_Inc"). Returns true + fills
    // out_json on success; false on unavailable / unknown type / parse fail.
    bool decode(const std::string& msg_type, const std::string& payload,
                std::string& out_json) const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sup_gui
