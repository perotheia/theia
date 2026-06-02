// User do_* bodies for the runnable node SupervisorWorker.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for this
// file's existence and refuses to overwrite unless `--force` is passed.
// Bodies are yours; the declarations are in lib/SupervisorWorker.hh.
//
// SupervisorWorker is the OS side of the supervisor: it owns the
// fork/exec/reap/watchdog/signals engine (impl/core/runtime.*) and runs its
// select() loop. do_loop() == Supervisor::run(). The engine is published to
// the process-global bridge so the sibling SupervisorCtl node can post control
// ops into it; outbound events leave via the EmitSink, which we point at the
// bridge's EmitForwarder (installed by SupervisorCtl).

#include "lib/SupervisorWorker.hh"

#include "core/bridge.h"
#include "core/runtime.h"
#include "core/spec.h"

#include "NodeRef.hh"          // theia::runtime::cast(self, msg, TipcAddr)
#include "GenServer.hh"        // THEIA_DECLARE_REMOTE_CODEC for the runtime msgs
#include "platform_runtime/runtime.pb.h"  // platform_runtime_TraceControlPush etc.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace ara::exec {

namespace {

}  // namespace

namespace {

// The supervision tree (JSON, emitted by `artheia executor emit`) and the
// child working-dir root come from the environment — the rig sets these when
// it launches the supervisor binary. Sensible fallbacks keep a bare run alive.
std::string manifest_path() {
    if (const char* p = std::getenv("THEIA_SUPERVISOR_MANIFEST"); p && *p) {
        return p;
    }
    return "supervisor_tree.json";  // cwd-relative default
}

std::string root_dir() {
    if (const char* d = std::getenv("THEIA_ROOT_DIR"); d && *d) {
        return d;
    }
    return ".";
}

// The engine the worker thread owns for the lifetime of do_loop(). Stored as a
// node member would be cleaner, but the generated lib header is regenerated, so
// we keep it file-local and publish it to the bridge instead.
std::unique_ptr<::supervisor::Supervisor> g_engine;

// Wire the engine's EmitSink to the bridge's EmitForwarder. Each callback is a
// best-effort deferral: if SupervisorCtl hasn't installed its forwarder yet
// (node construction order), the event is dropped.
::supervisor::EmitSink make_sink(SupervisorWorker& self) {
    using namespace ::supervisor;
    EmitSink s;
    s.on_event = [](const EventData& e) {
        if (auto fn = emit_forwarder().on_event) fn(e);
    };
    s.on_health = [](const HealthData& h) {
        if (auto fn = emit_forwarder().on_health) fn(h);
    };
    s.on_snapshot_begin = [](uint64_t gen, uint64_t ts) {
        if (auto fn = emit_forwarder().on_snapshot_begin) fn(gen, ts);
    };
    s.on_edge = [](const EdgeData& ed) {
        if (auto fn = emit_forwarder().on_edge) fn(ed);
    };
    s.on_node_state = [](const NodeStateData& ns) {
        if (auto fn = emit_forwarder().on_node_state) fn(ns);
    };
    s.on_snapshot_end = [](uint64_t gen) {
        if (auto fn = emit_forwarder().on_snapshot_end) fn(gen);
    };
    // Outbound config push to a child node, as the engine's typed INTENT. The
    // engine (transport-free) handed us the resolved (type,instance) + the typed
    // values; here — in the FC shell that links platform/runtime — we build the
    // REAL platform_runtime_TraceControlPush / LogLevelPush and cast it via the
    // runtime RemoteCodec (service_id + wire bytes match the child's
    // register_cast<Msg> by construction). No GwHdrWire, no hand encode.
    s.on_trace_push = [&self](uint32_t type, uint32_t instance,
                              uint32_t kind, bool enabled) {
        platform_runtime_TraceControlPush m{};
        m.kind = static_cast<platform_runtime_TraceKind>(kind);
        m.enabled = enabled;
        // GenRunnable's cast-to-remote override: builds the RemoteRef from the
        // engine-resolved (executor.json) child address and casts the typed msg.
        self.cast(m, ::theia::runtime::TipcAddr{type, instance});
    };
    s.on_log_push = [&self](uint32_t type, uint32_t instance, uint32_t level) {
        platform_runtime_LogLevelPush m{};
        m.level = static_cast<platform_runtime_LogLevelValue>(level);
        self.cast(m, ::theia::runtime::TipcAddr{type, instance});
    };
    return s;
}

}  // namespace


// One-time setup on the worker thread, before do_loop(). Build the engine from
// the manifest, install the emit sink, publish it to the bridge.
void SupervisorWorker::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    const std::string manifest = manifest_path();
    const std::string root     = root_dir();
    try {
        auto tree = ::supervisor::load_manifest(manifest);
        g_engine  = std::make_unique<::supervisor::Supervisor>(
            std::move(tree), root);
        g_engine->set_emit_sink(make_sink(*this));
        ::supervisor::set_supervisor(g_engine.get());
        std::fprintf(stderr,
                     "[%s] engine built from %s (root_dir=%s)\n",
                     kNodeName, manifest.c_str(), root.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[%s] FATAL: cannot build supervision engine: %s\n",
                     kNodeName, e.what());
        // Leave g_engine null — do_loop() returns immediately and the
        // process exits via the runtime's normal shutdown.
    }
}

// The body. Runs the engine's select() loop until request_shutdown() makes
// run() return (do_stop sets that). Beats the watchdog implicitly: the engine
// IS the watchdog, so there's nothing to report to.
void SupervisorWorker::do_loop() {
    if (!g_engine) {
        std::fprintf(stderr, "[%s] no engine — loop exits immediately\n",
                     kNodeName);
        return;
    }
    int rc = g_engine->run();
    std::fprintf(stderr, "[%s] engine loop exited (rc=%d)\n", kNodeName, rc);
}

// Release + signal do_loop() to return. stop_requested() is already set by the
// base; tell the engine's loop to wind down (it wakes via the command eventfd).
void SupervisorWorker::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    if (g_engine) g_engine->request_shutdown();
    ::supervisor::set_supervisor(nullptr);
}

}  // namespace ara::exec
