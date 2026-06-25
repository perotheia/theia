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

// No Theia-runtime transport headers here: the worker is a bare thread and only
// DEFERS config pushes to the forwarder (SupervisorCtl does the actual cast).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <sys/stat.h>   // stat / S_ISDIR — child-launch root = $prefix/current?

namespace ara::exec {

namespace {

}  // namespace

namespace {

// The supervision tree (JSON, emitted by `artheia executor emit`) and the
// child working-dir root come from the environment — the rig sets these when
// it launches the supervisor binary. THEIA_SUPERVISOR_MANIFEST is REQUIRED:
// there is no fallback. A missing env var means the launcher is wrong (e.g. a
// stale run-supervisor.sh, or a bare `./supervisor` with no environment), and
// the supervisor must NOT start with a guessed cwd-relative default — that
// silently masks the real misconfiguration. Fail loudly instead (do_start
// catches the throw and aborts with the message below).
std::string manifest_path() {
    const char* p = std::getenv("THEIA_SUPERVISOR_MANIFEST");
    if (p && *p) {
        return p;
    }
    throw std::runtime_error(
        "THEIA_SUPERVISOR_MANIFEST is not set — the supervisor needs an "
        "explicit executor manifest path in the environment (the launcher, "
        "e.g. run-supervisor.sh, exports it). Refusing to start.");
}

// The PREFIX the launcher exports: where the supervisor binary + manifest live.
// THEIA_ROOT_DIR is REQUIRED — the launcher (run-supervisor.sh / systemd /
// `theia start`) must export it. No cwd fallback, no self-locate: a deployed
// embedded supervisor that guesses its root is a latent bug. Missing → refuse
// to start, exactly like THEIA_SUPERVISOR_MANIFEST below.
std::string prefix_dir() {
    const char* d = std::getenv("THEIA_ROOT_DIR");
    if (d && *d) return d;
    throw std::runtime_error(
        "THEIA_ROOT_DIR is not set — the launcher must export it (e.g. "
        "THEIA_ROOT_DIR=/opt/theia). Refusing to start.");
}

// The directory children's relative start_cmd (`./bin/<svc>`) resolves against.
// SPLIT FROM the supervisor's own prefix (OTA design): the supervisor binary is
// the UPDATER — it lives at $THEIA_ROOT_DIR/bin and must NOT swap under itself —
// but the SERVICES + APPS it forks run from the `current` release symlink, which
// OTA (Mender theia-release / UCM) flips + rolls back. The child-launch root is
// ALWAYS $THEIA_ROOT_DIR/current — the release-dir layout is the ONLY layout (the
// launcher, incl. `theia start` + tests, lays a `current` symlink; there is no
// flat-bin fallback). A Mender `current`→releases/<ver> flip + the UCM-driven FC
// restart brings children up on the new version while the supervisor is untouched.
// REQUIRE the symlink — a missing `current` means a broken provision, refuse to
// start (same posture as a missing THEIA_ROOT_DIR / manifest).
std::string root_dir() {
    const std::string cur = prefix_dir() + "/current";
    struct stat st;
    if (::stat(cur.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return cur;
    throw std::runtime_error(
        "no release at " + cur + " — the supervisor launches each child's "
        "./bin/<svc> from $THEIA_ROOT_DIR/current; the launcher (provision / "
        "theia start) must lay that symlink. Refusing to start.");
}

// The engine the worker thread owns for the lifetime of do_loop(). Stored as a
// node member would be cleaner, but the generated lib header is regenerated, so
// we keep it file-local and publish it to the bridge instead.
std::unique_ptr<::supervisor::Supervisor> g_engine;

// Wire the engine's EmitSink to the bridge's EmitForwarder. Each callback is a
// best-effort deferral: if SupervisorCtl hasn't installed its forwarder yet
// (node construction order), the event is dropped.
::supervisor::EmitSink make_sink() {
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
    // Config push to a child — DEFER to the forwarder, do NOT cast here. The
    // worker is a bare thread (GenRunnable), NOT backed by the Theia runtime, so
    // it must not touch TIPC transport. SupervisorCtl (a runtime-backed
    // GenServer) installs the forwarder and does the actual cast of the runtime
    // TraceControlPush / LogLevelPush — same defer pattern as on_event/on_health.
    s.set_trace = [](const std::string& child, uint32_t kind, bool enabled) {
        if (auto fn = emit_forwarder().set_trace) fn(child.c_str(), kind, enabled);
    };
    s.set_log_level = [](const std::string& child, uint32_t level) {
        if (auto fn = emit_forwarder().set_log_level) fn(child.c_str(), level);
    };
    // OTP pg:monitor membership push — DEFER to the forwarder (same reason as
    // set_trace: the worker is a bare thread, only SupervisorCtl may cast). Flatten
    // the member pairs into [t0,i0,t1,i1,…] for the capture-free function pointer.
    s.push_pg_membership = [](uint32_t wt, uint32_t wi,
                              const std::string& group, uint32_t gtype,
                              const std::vector<std::pair<uint32_t,uint32_t>>& mem) {
        auto fn = emit_forwarder().push_pg_membership;
        if (!fn) return;
        std::vector<uint32_t> flat;
        flat.reserve(mem.size() * 2);
        for (const auto& [t, i] : mem) { flat.push_back(t); flat.push_back(i); }
        fn(wt, wi, group.c_str(), gtype, flat.data(),
           static_cast<uint32_t>(mem.size()));
    };
    return s;
}

}  // namespace


// One-time setup on the worker thread, before do_loop(). Build the engine from
// the manifest, install the emit sink, publish it to the bridge.
void SupervisorWorker::do_start() {
    this->log().info("runnable starting");

    const std::string root = root_dir();
    try {
        // manifest_path() THROWS if THEIA_SUPERVISOR_MANIFEST is unset (no
        // fallback — a guessed default would mask a launcher misconfig). The
        // Manifest ctor then loads + validates the JSON, THROWING if the file
        // is missing/malformed. A supervisor with no manifest cannot supervise
        // — that's fatal, not a soft-fail: we let the throw abort the process
        // (std::abort below) rather than limp on with a null engine.
        const std::string manifest = manifest_path();
        ::supervisor::Manifest m(manifest);
        g_engine = std::make_unique<::supervisor::Supervisor>(
            m.take_tree(), root);
        g_engine->set_emit_sink(make_sink());
        // Engine writes through THIS node's logger so its lines wear the
        // [#supervisor_worker] tag. &this->log() outlives the engine (the node
        // owns the logger; the engine is destroyed first in do_stop/teardown).
        g_engine->set_logger(&this->log());
        ::supervisor::set_supervisor(g_engine.get());
        this->log().info("engine built from " + manifest +
                         " (root_dir=" + root + ")");
    } catch (const std::exception& e) {
        // Startup failure is the one case that goes straight to stderr (the
        // logger may itself be the thing that's misconfigured) before aborting.
        std::fprintf(stderr,
                     "[%s] FATAL: cannot build supervision engine: %s\n",
                     kNodeName, e.what());
        std::abort();  // no manifest, no supervision — crash, don't limp.
    }
}

// The body. Runs the engine's select() loop until request_shutdown() makes
// run() return (do_stop sets that). Beats the watchdog implicitly: the engine
// IS the watchdog, so there's nothing to report to.
void SupervisorWorker::do_loop() {
    if (!g_engine) {
        this->log().error("no engine — loop exits immediately");
        return;
    }
    int rc = g_engine->run();
    this->log().info("engine loop exited (rc=" + std::to_string(rc) + ")");
}

// Release + signal do_loop() to return. stop_requested() is already set by the
// base; tell the engine's loop to wind down (it wakes via the command eventfd).
void SupervisorWorker::do_stop() {
    this->log().info("runnable stopping");
    if (g_engine) g_engine->request_shutdown();
    ::supervisor::set_supervisor(nullptr);
}

}  // namespace ara::exec
