// trace_decoder_lib — dlopen wrapper around the PLUGGABLE trace decoders.
//
// The trace records the panel shows carry an opaque proto-wire-v3 payload +
// its message-type name (e.g. "system_apps_Inc"). The decoder .so's decode
// that to JSON via libprotobuf reflection — the same .so's rf-theia / rtdb
// use. The GUI links no protobuf-reflection of its own, so it dlopen()s the
// plugins lazily and the detail pane can show `{n: 2}` instead of a hex dump.
//
// PLUGGABLE MODEL: the decoder is split across multiple plugins —
//   libtrace_decoder_system.so  (framework: sm + other framework types)
//   libtrace_decoder_apps.so    (the consuming workspace's app types)
// each carrying its OWN process-global registry + its OWN `trace_decode`
// symbol. We dlopen EVERY `libtrace_decoder_*.so` we find and, to decode a
// record, try each plugin's `trace_decode` until one returns >0. We also read
// each plugin's `trace_decoder_release_ver()` and warn (stderr) if an app
// plugin's version disagrees with the framework system plugin's.
//
// Plugin-dir discovery, in order:
//   1. $THEIA_TRACE_DECODER_PATH — colon-separated dirs; every
//      libtrace_decoder_*.so in each is loaded.
//   2. Legacy single-.so envs ($THEIA_TRACE_DECODER) — one explicit plugin.
//   3. Well-known bazel-bin paths relative to cwd + the exe dir, for BOTH the
//      framework system plugin and the demo apps plugin.
// Absent → decode is simply unavailable and the panel falls back to hex.

#include "sup_gui/trace_decoder_lib.h"

#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace sup_gui {

namespace {

// trace_decode(msg_type, payload, len, out_json, out_cap) → json length (>0)
// or 0 (unknown type / parse fail / buffer too small; out_json carries an
// error string). The C ABI from platform/runtime/trace/trace_decoder.hh.
using trace_decode_fn = int (*)(const char*, const unsigned char*,
                                unsigned long, char*, unsigned long);
using release_ver_fn  = const char* (*)();

std::string exe_dir() {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string p(buf);
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

bool has_decoder_prefix(const std::string& name) {
    // libtrace_decoder_<x>.so — the plugin naming convention.
    return name.rfind("libtrace_decoder_", 0) == 0 &&
           name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0;
}

// Append every libtrace_decoder_*.so in `dir` (absolute paths) to `out`.
void scan_dir(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (!has_decoder_prefix(name)) continue;
        out.emplace_back(dir + "/" + name);
    }
    ::closedir(d);
}

void split_colon(const char* s, std::vector<std::string>& out) {
    if (!s || !*s) return;
    std::string cur;
    for (const char* p = s; ; ++p) {
        if (*p == ':' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
}

}  // namespace

struct TraceDecoderLib::Impl {
    struct Plugin {
        void*           handle = nullptr;
        trace_decode_fn fn     = nullptr;
        std::string     path;
        std::string     ver;
    };
    std::vector<Plugin> plugins;
};

TraceDecoderLib::TraceDecoderLib() : impl_(new Impl()) {
    // Relative bazel-bin locations for the two known plugins (cwd = workspace
    // root, or a couple of climbs, or relative to the exe).
    static const char* kSystemRel =
        "bazel-bin/platform/runtime/trace/libtrace_decoder_system.so";
    static const char* kAppsRel =
        "bazel-bin/trace/libtrace_decoder_apps.so";

    // 1. Collect candidate .so paths.
    std::vector<std::string> candidates;

    // 1a. THEIA_TRACE_DECODER_PATH — colon-separated plugin DIRS.
    {
        std::vector<std::string> dirs;
        split_colon(std::getenv("THEIA_TRACE_DECODER_PATH"), dirs);
        for (const auto& dir : dirs) scan_dir(dir, candidates);
    }

    // 1b. Legacy single-.so env — treat as one explicit plugin.
    if (const char* e = std::getenv("THEIA_TRACE_DECODER"); e && *e)
        candidates.emplace_back(e);

    // 1c. Well-known bazel-bin paths for BOTH plugins.
    std::vector<std::string> roots = {".", "..", "../..", "../../.."};
    if (const std::string d = exe_dir(); !d.empty()) {
        roots.push_back(d);
        roots.push_back(d + "/../../../..");
    }
    for (const auto& r : roots) {
        candidates.emplace_back(r + "/" + kSystemRel);
        candidates.emplace_back(r + "/" + kAppsRel);
    }

    // 2. dlopen each unique resolvable candidate, dedup by realpath.
    std::set<std::string> seen;
    for (const auto& path : candidates) {
        char real[PATH_MAX];
        const char* key = ::realpath(path.c_str(), real) ? real : path.c_str();
        if (!seen.insert(key).second) continue;

        void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        auto fn = reinterpret_cast<trace_decode_fn>(::dlsym(h, "trace_decode"));
        if (!fn) { ::dlclose(h); continue; }

        Impl::Plugin p;
        p.handle = h;
        p.fn = fn;
        p.path = key;
        if (auto vf = reinterpret_cast<release_ver_fn>(
                ::dlsym(h, "trace_decoder_release_ver"))) {
            if (const char* v = vf()) p.ver = v;
        }
        impl_->plugins.push_back(std::move(p));
    }

    // Summary: which decoder plugins loaded (so "my app types show as hex"
    // is one log line away from "the apps plugin didn't load"). One line.
    if (impl_->plugins.empty()) {
        std::fprintf(stderr, "[trace_decoder_lib] no decoder plugins found "
            "(set THEIA_TRACE_DECODER_PATH) — payloads render as hex.\n");
    } else {
        std::fprintf(stderr, "[trace_decoder_lib] loaded %zu decoder plugin(s):\n",
                     impl_->plugins.size());
        for (const auto& p : impl_->plugins)
            std::fprintf(stderr, "  %s%s%s\n", p.path.c_str(),
                         p.ver.empty() ? "" : " ver=", p.ver.c_str());
    }

    // 3. Version-compat WARNING: the FIRST plugin whose name carries
    // "_system" is the framework reference; warn if any other plugin's
    // version disagrees. (Soft check — never fails decode.)
    std::string fw_ver;
    for (const auto& p : impl_->plugins)
        if (p.path.find("libtrace_decoder_system.so") != std::string::npos) {
            fw_ver = p.ver;
            break;
        }
    if (!fw_ver.empty()) {
        for (const auto& p : impl_->plugins) {
            if (p.path.find("libtrace_decoder_system.so") != std::string::npos)
                continue;
            if (!p.ver.empty() && p.ver != fw_ver) {
                std::fprintf(stderr,
                    "[trace_decoder_lib] WARNING: plugin %s release_ver=%s "
                    "differs from framework system plugin %s — wire format may "
                    "have drifted.\n",
                    p.path.c_str(), p.ver.c_str(), fw_ver.c_str());
            }
        }
    }
}

TraceDecoderLib::~TraceDecoderLib() {
    for (auto& p : impl_->plugins)
        if (p.handle) ::dlclose(p.handle);
    delete impl_;
}

bool TraceDecoderLib::available() const { return !impl_->plugins.empty(); }

bool TraceDecoderLib::decode(const std::string& msg_type,
                             const std::string& payload,
                             std::string& out_json) const {
    if (impl_->plugins.empty() || payload.empty()) return false;
    char buf[4096];
    // Try each plugin until one decodes the type (returns >0).
    for (const auto& p : impl_->plugins) {
        const int n = p.fn(
            msg_type.c_str(),
            reinterpret_cast<const unsigned char*>(payload.data()),
            static_cast<unsigned long>(payload.size()),
            buf, sizeof(buf));
        if (n > 0) {
            out_json.assign(buf, static_cast<size_t>(n));
            return true;
        }
    }
    return false;                      // no plugin knew the type / parse fail
}

}  // namespace sup_gui
