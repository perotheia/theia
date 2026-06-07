// trace_decoder_lib — dlopen wrapper around libtrace_decoder_ctypes.so.
//
// The trace records the panel shows carry an opaque proto-wire-v3 payload +
// its message-type name (e.g. "system_demo_Inc"). libtrace_decoder.so (built
// by //platform/runtime/trace:libtrace_decoder_ctypes.so) decodes that to
// JSON via libprotobuf reflection — the same .so rf-theia / rtdb use. We
// dlopen it lazily (the GUI links no protobuf-reflection of its own) so the
// detail pane can show `{n: 2}` instead of a hex dump.
//
// Locating the .so: $THEIA_TRACE_DECODER wins; else the well-known bazel-bin
// path relative to a few plausible roots (cwd, the exe dir). Absent → decode
// is simply unavailable and the panel falls back to the hex view.

#include "sup_gui/trace_decoder_lib.h"

#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

// trace_decode(msg_type, payload, len, out_json, out_cap) → json length (>0)
// or 0 (unknown type / parse fail / buffer too small; out_json carries an
// error string). The C ABI from platform/runtime/trace/trace_decoder.hh.
using trace_decode_fn = int (*)(const char*, const unsigned char*,
                                unsigned long, char*, unsigned long);

std::string exe_dir() {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string p(buf);
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

}  // namespace

struct TraceDecoderLib::Impl {
    void*           handle = nullptr;
    trace_decode_fn fn     = nullptr;
};

TraceDecoderLib::TraceDecoderLib() : impl_(new Impl()) {
    static const char* kRel =
        "bazel-bin/platform/runtime/trace/libtrace_decoder_ctypes.so";

    std::vector<std::string> candidates;
    if (const char* e = std::getenv("THEIA_TRACE_DECODER"); e && *e)
        candidates.emplace_back(e);
    // cwd-relative (running from the workspace root) + a couple of climbs.
    candidates.emplace_back(kRel);
    candidates.emplace_back(std::string("../") + kRel);
    candidates.emplace_back(std::string("../../") + kRel);
    if (const std::string d = exe_dir(); !d.empty()) {
        candidates.emplace_back(d + "/" + kRel);
        candidates.emplace_back(d + "/../../../../" + kRel);
    }

    for (const auto& path : candidates) {
        void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        auto fn = reinterpret_cast<trace_decode_fn>(
            ::dlsym(h, "trace_decode"));
        if (!fn) { ::dlclose(h); continue; }
        impl_->handle = h;
        impl_->fn = fn;
        break;
    }
}

TraceDecoderLib::~TraceDecoderLib() {
    if (impl_->handle) ::dlclose(impl_->handle);
    delete impl_;
}

bool TraceDecoderLib::available() const { return impl_->fn != nullptr; }

bool TraceDecoderLib::decode(const std::string& msg_type,
                             const std::string& payload,
                             std::string& out_json) const {
    if (!impl_->fn || payload.empty()) return false;
    char buf[4096];
    const int n = impl_->fn(
        msg_type.c_str(),
        reinterpret_cast<const unsigned char*>(payload.data()),
        static_cast<unsigned long>(payload.size()),
        buf, sizeof(buf));
    if (n <= 0) return false;          // unknown type / parse fail
    out_json.assign(buf, static_cast<size_t>(n));
    return true;
}

}  // namespace sup_gui
