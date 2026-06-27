// User do_* bodies for the runnable node ComGrpcProxy.
//
// FIRST-TIME-ONLY SCAFFOLD origin: `artheia gen-app --kind fc`. Bodies are
// ours; the declarations live in lib/ComGrpcProxy.hh (byte-stable, do NOT add
// members there). All gRPC state is file-static in this translation unit —
// ComGrpcProxy is a single-instance runnable (one per com process), so a
// pimpl-via-file-statics keeps the generated header untouched.
//
// The old hand-rolled World-A gRPC main() folded into World B: this is now a
// first-class generated runnable node, built by the native Bazel binary
// //services/com/main:com (no more cmake / rules_foreign_cc). com's job:
// bridge external gRPC peers (dashboards / tdb / rf-theia) to the on-host
// supervisor. We serve `SupervisorView` over gRPC.
//
// #418 — CONTROL path (the unary mutators: Start/Delete/Restart/Terminate/
// Stop/ConfigureLogLevel/ConfigureTrace/GetTraceConfig) goes over the STANDARD
// Theia transport: SupLink (impl/sup_link) holds a RemoteRef to the
// supervisor's gen_server control node (TIPC type 0x80020003/0) and does a
// typed nanopb CALL. Each RPC translates its libprotobuf gRPC args into the
// SupLink primitives, gets a SupReply back, and fills the libprotobuf reply.
//
// FIREHOSE path (Subscribe: live tree stream) is a GetTree POLL, not a push.
// The supervisor's in-process event firehose (broadcast_events_edge) has NO
// remote egress — it never crosses TIPC to com. So Subscribe mirrors
// `tdb ps --follow`: poll SupLink::get_tree() on an interval (THEIA_COM_POLL_MS,
// default 1s) and emit each TreeSnapshot as a `snapshot` observation. GetTree
// is the live source of truth — the same call `tdb ps` uses one-shot. The gRPC
// client diffs successive snapshots if it wants deltas; com does not fabricate
// event/health frames from a feed that never arrives. The gRPC edge stays
// libprotobuf; nanopb is confined to impl/sup_link.cc.
//
// BUILD NOTE: grpc++ + the supervisor bridge .pb/.grpc.pb come from native
// Bazel targets — the //services/com:com_bridge_grpc cc_library (host protoc +
// grpc_cpp_plugin genrule, grpc++ via system linkopts) and
// //platform/supervisor:supervisor_pb_cpp (the libprotobuf C++ bindings). The
// process therefore links BOTH libprotobuf (gRPC edge) AND libprotobuf-nanopb
// (ComDaemon's TIPC wire) — the deliberate two-codec, one-process design.

#include "lib/ComGrpcProxy.hh"

#include "impl/sup_link.hpp"      // #418 control path over the standard transport
#include "impl/per_link.hpp"      // per (persistency) proxy — ListSchemas/Snapshot
#include "impl/nm_link.hpp"       // nm (network mgmt) proxy — GetStatus/WifiScan
#include "impl/diag_link.hpp"     // diag (DoIP/UDS) proxy — SendUds
#include "impl/ucm_link.hpp"      // ucm (ara::ucm) proxy — RequestUpdate + progress
#include "impl/vucm_link.hpp"     // vucm (L4-B vehicle campaign) proxy — CheckForCampaign
#include "impl/shwa_link.hpp"     // SHWA AccelTelemetry egress receiver (GPU/host)
#include "impl/machine_manifest.hpp"  // cluster index→name map (per-machine label)
#include "impl/com_tls.hpp"       // shared TLS-or-insecure ServerCredentials

#include "TipcTopology.hh"        // Stage 3: live supervisor-instance discovery
#include "PgClient.hh"            // PG egress for the FcHealthReport → PHM edge
#include "lib/com_codecs.hh"      // RemoteCodec<system_services_phm_FcHealthReport>
#include <pb_encode.h>

#include "supervisor_bridge.grpc.pb.h"

// The supervisor's wire types com bridges — now ONE consolidated libprotobuf
// header (package system_supervisor; ChildSelector, ChildSpec, ControlReply,
// SupervisionEvent, TraceConfigList, TreeSnapshot, …). The old per-message
// ChildSelector.pb.h / ControlRequest.pb.h / etc. are gone.
#include "supervisor.pb.h"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <sys/stat.h>   // mkdir/chmod for the provisioning creds dir
#include <vector>

namespace ara::com {

namespace {

// op_kind values shared with platform/supervisor/src/runtime.cpp.
constexpr uint32_t kOpStartChild        = 3;
constexpr uint32_t kOpDeleteChild       = 4;
constexpr uint32_t kOpRestartChild      = 5;
constexpr uint32_t kOpTerminateChild    = 6;
constexpr uint32_t kOpConfigureTrace    = 9;   // #361 (trace control path)
constexpr uint32_t kOpGetTraceConfig    = 10;  // crash-investigation read-back
constexpr uint32_t kOpConfigureLogLevel = 11;  // #385

// Listen address. Overridable via THEIA_COM_LISTEN (the rig/executor sets it
// from the machine manifest's com gRPC endpoint); defaults to the historical
// 0.0.0.0:7700 that supervisor-gui + tdb + rf-theia connect to.
std::string listen_addr() {
    if (const char* e = std::getenv("THEIA_COM_LISTEN")) return e;
    return "0.0.0.0:7700";
}

// ---- SHWA AccelTelemetry fold-in (gated at com) --------------------------
//
// SHWA broadcasts AccelSample over TIPC regardless of subscribers; com folds it
// into the SupervisorView.Subscribe firehose as the `accel` observation. The
// GATE mirrors LogForwarder's subscriber-count gate: ShwaLink's sink only stores
// the latest sample while ≥1 gRPC Subscribe client is connected
// (g_accel_subscribers > 0), so com does no AccelSample work — and emits nothing
// into any stream — without a connected subscriber ("no active forward without a
// subscription" at the com edge). Each connected Subscribe loop emits the latest
// stored sample on its poll tick, so the GUI's GPU/host panels fill live.
std::atomic<int>        g_accel_subscribers{0};
std::mutex              g_accel_mtx;
// PER-MACHINE latest sample, keyed by machine_index. Both machines' shwa push to
// the SAME egress name (0x8001001A) on the shared host TIPC namespace, so com's
// one receiver gets all of them interleaved; the AccelSample.machine_index field
// (stamped by shwa from its TIPC instance) is the demux key. Each Subscribe tick
// emits one `accel` arm per machine that has a fresh sample.
std::unordered_map<uint32_t, std::string> g_accel_latest;   // inst → raw bytes
std::atomic<uint64_t>   g_accel_seq{0};     // bumps on each new sample (any machine)

// ShwaLink sink — called on the recv thread for each AccelSample. GATE: drop the
// sample unless a gRPC subscriber is connected (the same gate as LogForwarder,
// which only fans out while a Subscribe RPC holds the link). Demux by machine:
// parse the sample's machine_index and store it under that key so a multi-machine
// cluster keeps each machine's latest HW telemetry separately.
void on_accel_sample(const std::string& sample_bytes) {
    if (g_accel_subscribers.load(std::memory_order_relaxed) <= 0) return;
    // Parse just enough to read machine_index (cheap; the GUI re-uses the bytes).
    services::com::AccelSample s;
    uint32_t inst = 0;
    if (s.ParseFromString(sample_bytes)) inst = s.machine_index();
    {
        std::lock_guard<std::mutex> lk(g_accel_mtx);
        g_accel_latest[inst] = sample_bytes;
    }
    g_accel_seq.fetch_add(1, std::memory_order_relaxed);
}

// ---- Stage 3: cluster-wide supervisor fan-out ----------------------------
//
// com on central aggregates EVERY present machine's supervisor tree into ONE
// SupervisorView stream, so a single rtdb/GUI against central:7700 sees the
// whole cluster. The supervisor control node binds TIPC type 0x80020001 at a
// per-machine instance (central=0, compute=1, …). A TipcTopology subscribed to
// {0x80020001, 0..kMaxSupInstance} keeps the live set of present instances; the
// Subscribe poll fans get_tree() across them and MERGES the children.
//
// Instance 0 (the local supervisor) is ALWAYS the back-compat path: control
// RPCs and a lone-machine stack behave exactly as before. Remote instances are
// only ever touched while a Subscribe client is connected (the poll loop).
//
// kSupCtlTipcType / kMaxSupInstance come from sup_link.hpp (shared with the
// SupLink that owns the per-instance connections) — one source of truth.
using services_com::kSupCtlTipcType;
using services_com::kMaxSupInstance;

theia::runtime::TipcTopology g_sup_topology;
std::atomic<bool>            g_topology_up{false};

// The machine-tag prefix for an instance: "<machine_name>/" from the manifest
// (central=0 → "central/", compute=1 → "compute/"); falls back to "mN/" when the
// manifest is absent. EVERY instance — including 0 — is name-tagged so each
// machine is a first-class NAMED subtree (the GUI/rtdb key on the real machine
// name, not a synthetic mN or the connection host). A real node name never
// contains '/', so the prefix can't collide with one; rtdb rebuilds the forest
// by parent_name, so each machine's root (parent_name=="") becomes its own
// top-level subtree. route_target() resolves the prefix back to an instance via
// the manifest's name→index lookup.
// Forward decl — defined just below the registry (it reads g_machines first,
// then falls back to the manifest).
std::string machine_name_of(uint32_t inst);

std::string machine_prefix(uint32_t inst) {
    return machine_name_of(inst) + "/";
}

// ---- Machine registry — the cluster scan's per-instance identity cache -------
//
// com discovers supervisors over the TIPC topology scan (g_sup_topology); on
// each PUBLISH it fetches that instance's GetSystemInfo (static host identity:
// hostname/kernel/OS/git-sha/supervisor-start) and caches it HERE, keyed by
// instance. This is the authoritative per-machine identity the GUI / rtdb read
// via ListMachines — recovered from the supervisor itself, not just inferred
// from a name-prefix. A WITHDRAWN machine keeps its last-known identity with
// present=false (so it's still listed; it just shows offline). Live host metrics
// (disk/uptime/ram/gpu) ride the SHWA AccelSample arm of Subscribe — this holds
// the STATIC identity only.
struct MachineEntry {
    bool        present = false;
    std::string name;          // machine name REPORTED BY the supervisor (SystemInfo
                               // .machine_name from executor.json); authoritative
                               // over the manifest/"mN" fallback. "" until fetched.
    std::string system_info;   // raw system_supervisor::SystemInfo bytes ("" until fetched)
};
std::mutex                                   g_machines_mtx;
std::unordered_map<uint32_t, MachineEntry>   g_machines;     // instance → entry

// The machine NAME for an instance: prefer the supervisor-REPORTED name (cached
// off GetSystemInfo, sourced from executor.json), else the manifest, else "mN".
// This is why a single-machine dev stack with no THEIA_MACHINE_MANIFEST still
// shows "central": the supervisor tells com its own name.
std::string machine_name_of(uint32_t inst) {
    {
        std::lock_guard<std::mutex> lk(g_machines_mtx);
        auto it = g_machines.find(inst);
        if (it != g_machines.end() && !it->second.name.empty())
            return it->second.name;
    }
    return services_com::MachineManifest::instance().name(inst);
}

// Mark an instance present/absent (called from the topology callback). The
// identity (system_info) is filled separately by cache_machine_sysinfo so a
// WITHDRAWN→PUBLISHED flap keeps the cached facts.
void set_machine_present(uint32_t inst, bool present) {
    std::lock_guard<std::mutex> lk(g_machines_mtx);
    g_machines[inst].present = present;
}

// Fetch + cache one instance's GetSystemInfo (best-effort; called on a detached
// thread off the topology PUBLISH so a slow/wedged supervisor never stalls the
// scan callback). Stores the raw SystemInfo bytes under the instance.
void cache_machine_sysinfo(uint32_t inst) {
    services_com::SupReply r;
    auto& link = services_com::SupLink::for_instance(inst);
    if (!link.connected() && !link.start(/*connect_timeout_ms=*/1500)) return;
    if (!link.get_system_info(r, /*timeout_ms=*/2000) || r.system_info.empty())
        return;
    // The supervisor reports its OWN machine name (executor.json root "machine")
    // in SystemInfo.machine_name — the authoritative source. Extract it so the
    // registry names the machine even with no THEIA_MACHINE_MANIFEST.
    std::string reported;
    {
        system_supervisor::SystemInfo si;
        if (si.ParseFromString(r.system_info) && !si.machine_name().empty())
            reported = si.machine_name();
    }
    {
        std::lock_guard<std::mutex> lk(g_machines_mtx);
        g_machines[inst].system_info = r.system_info;
        if (!reported.empty()) g_machines[inst].name = reported;
    }
    std::fprintf(stderr, "[com] cached host identity for machine %u (%s)\n",
                 inst, machine_name_of(inst).c_str());
}

// Resolve a machine NAME (the selector on Subscribe / GetSystemInfo) → instance.
// Checks the supervisor-reported registry names FIRST (so "central" resolves
// even without a manifest), then the manifest's name→index (incl. the "mN"
// synthetic form). Returns false for an unknown name.
bool machine_name_to_instance(const std::string& name, uint32_t& inst) {
    {
        std::lock_guard<std::mutex> lk(g_machines_mtx);
        for (const auto& kv : g_machines)
            if (kv.second.name == name) { inst = kv.first; return true; }
    }
    return services_com::MachineManifest::instance().index_of(name, inst);
}

// One present supervisor's children, machine-tagged, appended to `merged`.
// Returns true if it contributed a parseable, non-empty tree. `newest_gen` /
// `newest_ts` accumulate the freshest generation/timestamp across machines.
//
// timeout_ms is the per-machine call budget: the LOCAL supervisor (inst 0) gets
// the full budget, but a REMOTE machine gets a TIGHT one so a single wedged /
// slow-to-answer peer can't stall the whole cluster poll (the Subscribe loop
// must keep ticking for the machines that ARE healthy). A remote miss just drops
// that machine from this snapshot; it reappears on the next tick once it answers.
bool fold_instance_tree(uint32_t inst, system_supervisor::TreeSnapshot* merged,
                        uint64_t& newest_gen, uint64_t& newest_ts,
                        int timeout_ms) {
    auto& link = services_com::SupLink::for_instance(inst);
    // Lazily connect a remote link the first time it's seen (instance 0 is
    // already started in do_start). Cheap: only happens once per machine. A
    // WITHDRAWN machine that comes back keeps this same link (re-PUBLISH reuses
    // it); a still-gone machine just fails get_tree below and is skipped.
    if (!link.connected()) {
        if (!link.start(/*connect_timeout_ms=*/1500)) return false;
    }
    services_com::SupReply r;
    if (!link.get_tree(r, timeout_ms) || r.tree_snapshot.empty()) return false;
    system_supervisor::TreeSnapshot one;
    if (!one.ParseFromString(r.tree_snapshot)) return false;

    if (one.generation()   > newest_gen) newest_gen = one.generation();
    if (one.timestamp_ms() > newest_ts)  newest_ts  = one.timestamp_ms();

    const std::string pfx = machine_prefix(inst);   // "<machine_name>/"
    for (const auto& ch : one.children()) {
        auto* dst = merged->add_children();
        *dst = ch;
        dst->set_name(pfx + ch.name());
        // Re-anchor parent: the per-machine root (parent_name=="") stays a
        // top-level node so each machine renders as its own subtree.
        if (!ch.parent_name().empty())
            dst->set_parent_name(pfx + ch.parent_name());
    }
    return true;
}

// Merge every PRESENT supervisor's tree into one TreeSnapshot so a single
// rtdb/GUI against central:7700 sees the whole cluster. Instance 0 (the local
// supervisor) is ALWAYS tried — even before topology fires — so a single-machine
// stack works the instant the local sup binds (and looks byte-identical to the
// pre-cluster behaviour). Instances ≥1 come from TipcTopology and are mN/-tagged.
//
// We tag by NAME rather than adding a machine field to TreeSnapshot: the instance
// already disambiguates the machine, and name-prefixing needs ZERO wire/proto/rtdb
// change (no supervisor-proto regen hazard) while still making the machine visible
// in `rtdb ps`. Returns false if NO instance yielded a tree.
bool merge_present_trees(system_supervisor::TreeSnapshot* merged) {
    // Per-machine call budgets: local gets the full default, remotes a tight
    // one so one slow peer can't stall the cluster poll (see fold_instance_tree).
    constexpr int kLocalTreeTimeoutMs  = 3000;
    constexpr int kRemoteTreeTimeoutMs = 800;

    // Present remote instances from topology; always include 0.
    std::vector<uint32_t> insts;
    if (g_topology_up.load(std::memory_order_relaxed)) {
        insts = g_sup_topology.instances_of(kSupCtlTipcType);
    }
    if (std::find(insts.begin(), insts.end(), 0u) == insts.end())
        insts.insert(insts.begin(), 0u);
    std::sort(insts.begin(), insts.end());

    bool any = false;
    uint64_t newest_gen = 0, newest_ts = 0;
    for (uint32_t inst : insts) {
        const int budget = (inst == 0) ? kLocalTreeTimeoutMs
                                       : kRemoteTreeTimeoutMs;
        any |= fold_instance_tree(inst, merged, newest_gen, newest_ts, budget);
    }
    merged->set_generation(newest_gen);
    merged->set_timestamp_ms(newest_ts);
    return any;
}

// Single-machine tree: fold ONLY the named machine's supervisor (the explicit
// Subscribe.machine / `rtdb ps <machine>` selector). Resolves the name → its
// TIPC instance and folds just that one — deterministic, no interleaving, no
// dependence on which peer answered first. Returns false (empty snapshot) for an
// unknown machine or one that doesn't answer this tick.
bool fold_named_machine(const std::string& machine,
                        system_supervisor::TreeSnapshot* merged) {
    uint32_t inst = 0;
    if (!machine_name_to_instance(machine, inst)) return false;
    // The selected machine gets the full local budget (the client asked for
    // exactly this one — no other peer to protect from a slow answer).
    uint64_t newest_gen = 0, newest_ts = 0;
    bool any = fold_instance_tree(inst, merged, newest_gen, newest_ts,
                                  /*timeout_ms=*/3000);
    merged->set_generation(newest_gen);
    merged->set_timestamp_ms(newest_ts);
    return any;
}

// ---- gRPC service: forwards control RPCs onto the supervisor uplink ------
class SupervisorViewImpl final
    : public services::com::SupervisorView::Service {
public:
    SupervisorViewImpl() = default;

    // ---- Streaming firehose — GetTree POLL (the pull model) --------------
    // The supervisor's in-process event firehose (broadcast_events_edge) has
    // no remote egress — it never reaches com over TIPC. So Subscribe mirrors
    // `tdb ps --follow`: poll GetTree on an interval and emit each TreeSnapshot
    // as a `snapshot` observation. GetTree is the live source of truth (same
    // call `tdb ps` uses one-shot). The client diffs successive snapshots if it
    // wants edge/health deltas; com no longer fabricates them from a dead feed.
    //
    // Interval: THEIA_COM_POLL_MS (default 1000ms), Ctrl-C/cancel-aware.
    grpc::Status Subscribe(
            grpc::ServerContext* ctx,
            const services::com::SubscribeRequest* req,
            grpc::ServerWriter<services::com::SupervisorObservation>* writer)
            override {
        int poll_ms = 1000;
        if (const char* e = std::getenv("THEIA_COM_POLL_MS")) {
            int v = std::atoi(e);
            if (v >= 50) poll_ms = v;
        }
        // Optional machine selector: "" → whole-cluster aggregate (legacy);
        // a name → that ONE machine's tree (deterministic per-machine view).
        // The accel arm is filtered to the same machine when a selector is set.
        const std::string sel_machine = req ? req->machine() : std::string();
        uint32_t sel_inst = 0;
        const bool has_sel =
            !sel_machine.empty() &&
            machine_name_to_instance(sel_machine, sel_inst);
        std::fprintf(stderr, "com: gRPC subscriber attached "
                     "(GetTree poll every %dms, machine=%s)\n",
                     poll_ms, sel_machine.empty() ? "*" : sel_machine.c_str());
        // GATE on: enable the SHWA AccelTelemetry fold-in while this subscriber
        // is connected (LogForwarder-mirrored subscriber-count gate). The recv
        // thread only stores samples while this is > 0.
        g_accel_subscribers.fetch_add(1, std::memory_order_relaxed);
        uint64_t last_accel_seq = 0;
        while (!ctx->IsCancelled()) {
            // Stage 3: ONE merged snapshot across every present machine's
            // supervisor (central=inst 0 + compute=inst 1 + …). On a single-
            // machine stack this is exactly the instance-0 tree as before.
            {
                services::com::SupervisorObservation obs;
                auto* s = obs.mutable_snapshot();
                const bool got = has_sel ? fold_named_machine(sel_machine, s)
                                         : merge_present_trees(s);
                if (got && s->children_size() > 0) {
                    if (!writer->Write(obs)) break;   // client gone
                }
            }
            // HealthBeacon: same poll cadence, separate observation (GUI Load
            // panel + "heartbeat" status). The supervisor's GetHealth returns
            // its latest beacon — no TIPC event-firehose subscription needed.
            services_com::SupReply hr;
            auto& health_link = has_sel
                ? services_com::SupLink::for_instance(sel_inst)
                : services_com::SupLink::instance();
            if (health_link.get_health(hr) && !hr.health.empty()) {
                services::com::SupervisorObservation obs;
                auto* h = obs.mutable_health();
                if (h->ParseFromString(hr.health)) {
                    if (!writer->Write(obs)) break;
                }
            }
            // SHWA hardware telemetry (GPU / host monitor): emit each MACHINE's
            // latest AccelSample as its own `accel` arm whenever a new one has
            // arrived since our last tick. The recv thread only populates
            // g_accel_latest while a subscriber is connected (the gate), so this
            // is a no-op when SHWA is absent or quiet. machine_name is filled
            // from the manifest so the GUI Load panel labels per-machine HW.
            {
                uint64_t seq = g_accel_seq.load(std::memory_order_relaxed);
                if (seq != last_accel_seq) {
                    std::vector<std::string> raws;
                    {
                        std::lock_guard<std::mutex> lk(g_accel_mtx);
                        raws.reserve(g_accel_latest.size());
                        for (const auto& kv : g_accel_latest)
                            raws.push_back(kv.second);
                    }
                    last_accel_seq = seq;
                    bool client_gone = false;
                    for (const auto& raw : raws) {
                        if (raw.empty()) continue;
                        services::com::SupervisorObservation obs;
                        auto* a = obs.mutable_accel();
                        if (!a->ParseFromString(raw)) continue;
                        // Drop samples from other machines when a single-machine
                        // selector is active (the per-machine view stays pure).
                        if (has_sel && a->machine_index() != sel_inst) continue;
                        // Label with the human machine name (supervisor-reported,
                        // else manifest, else "mN") for the index shwa stamped.
                        a->set_machine_name(machine_name_of(a->machine_index()));
                        if (!writer->Write(obs)) { client_gone = true; break; }
                    }
                    if (client_gone) break;
                }
            }
            // Sleep in short slices so cancellation is responsive.
            for (int slept = 0; slept < poll_ms && !ctx->IsCancelled();
                 slept += 100) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min(100, poll_ms - slept)));
            }
        }
        // GATE off: this subscriber is leaving. When the count hits 0 the recv
        // thread stops storing samples (no forward without a subscription).
        g_accel_subscribers.fetch_sub(1, std::memory_order_relaxed);
        std::fprintf(stderr, "com: gRPC subscriber detached\n");
        return grpc::Status::OK;
    }

    // ---- Unary mutators — #418 over the standard transport via SupLink -----
    grpc::Status StartChild(grpc::ServerContext*,
                            const services::com::StartChildCall* req,
                            system_supervisor::ControlReply* reply) override {
        const auto& gs = req->spec();
        services_com::SupChildSpec spec;
        // Route to the machine that owns the new child (mN/ on the name); the
        // parent_supervisor shares the same machine, so strip its prefix too.
        std::string parent_bare;
        auto& link = route_target(gs.name(), spec.name);
        (void)route_target(gs.parent_supervisor(), parent_bare);
        spec.parent_supervisor = parent_bare;
        spec.restart           = gs.restart();
        spec.shutdown          = gs.shutdown();
        spec.type              = gs.type();
        for (const auto& a : gs.start_cmd()) spec.start_cmd.push_back(a);
        for (const auto& m : gs.modules())   spec.modules.push_back(m);
        services_com::SupReply r;
        if (!link.start_child(spec, r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }
    grpc::Status DeleteChild(grpc::ServerContext*,
                             const services::com::DeleteChildCall* req,
                             system_supervisor::ControlReply* reply) override {
        return name_op(kOpDeleteChild, req->name(), reply);
    }
    grpc::Status RestartChild(grpc::ServerContext*,
                              const ::system_supervisor::ChildSelector* sel,
                              system_supervisor::ControlReply* reply) override {
        return name_op(kOpRestartChild, sel->name(), reply);
    }
    grpc::Status TerminateChild(grpc::ServerContext*,
                                const ::system_supervisor::ChildSelector* sel,
                                system_supervisor::ControlReply* reply) override {
        return name_op(kOpTerminateChild, sel->name(), reply);
    }

    // #385 — set a child's runtime log level. The supervisor stores it
    // (survives restart) and pushes it live. Now over the standard transport.
    grpc::Status ConfigureLogLevel(
            grpc::ServerContext*,
            const services::com::LogLevelCall* req,
            system_supervisor::ControlReply* reply) override {
        std::string bare;
        auto& link = route_target(req->target_node(), bare);
        services_com::SupReply r;
        if (!link.configure_log_level(bare, req->level(), r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }

    // ConfigureTrace — the trace CONTROL/ingress path (rf → com → supervisor
    // → node). The supervisor owns trace_config_[child] + the per-NODE push
    // (#361), so it survives restart. com is NOT in the trace EGRESS byte path
    // — records stream from the collector's own TraceStream gRPC (services/log).
    grpc::Status ConfigureTrace(
            grpc::ServerContext*,
            const services::com::TraceConfigRequest* req,
            system_supervisor::ControlReply* reply) override {
        std::string bare;
        auto& link = route_target(req->target_node(), bare);
        services_com::SupReply r;
        if (!link.configure_trace(bare, req->msg_type(), req->enabled(),
                req->kind(), r))   // #403: trace-kind selector → node
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }

    // Crash-investigation read-back. The supervisor carries its flattened
    // TraceConfigList INLINE in ControlReply.trace_config_list (single
    // correlated frame). We get the raw proto bytes back from SupLink and
    // unpack them into the caller's libprotobuf TraceConfigList.
    grpc::Status GetTraceConfig(
            grpc::ServerContext*,
            const services::com::GetTraceConfigCall*,
            system_supervisor::TraceConfigList* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_trace_config(r))
            return unavailable();
        // Empty list (no trace configured) is a valid, non-error result.
        if (!r.trace_config_list.empty() &&
            !out->ParseFromString(r.trace_config_list)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed TraceConfigList in ControlReply");
        }
        return grpc::Status::OK;
    }

    // Host facts read-back — backs `rtdb supervisor` / `info`. The supervisor
    // serves SystemInfo over TIPC; we get the raw bytes from SupLink and
    // unpack into the caller's libprotobuf SystemInfo.
    grpc::Status GetSystemInfo(
            grpc::ServerContext*,
            const services::com::GetSystemInfoCall* req,
            system_supervisor::SystemInfo* out) override {
        // Machine selector: "" → instance 0 (central, legacy). A name routes to
        // that instance's supervisor for ITS host identity. We serve from the
        // cluster-scan cache first (filled on discovery), falling back to a live
        // fetch if the cache is cold — so a known machine answers fast.
        const std::string machine = req ? req->machine() : std::string();
        uint32_t inst = 0;
        if (!machine.empty() && !machine_name_to_instance(machine, inst)) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "unknown machine: " + machine);
        }
        {   // cache hit?
            std::lock_guard<std::mutex> lk(g_machines_mtx);
            auto it = g_machines.find(inst);
            if (it != g_machines.end() && !it->second.system_info.empty()) {
                if (out->ParseFromString(it->second.system_info))
                    return grpc::Status::OK;
            }
        }
        services_com::SupReply r;
        if (!services_com::SupLink::for_instance(inst).get_system_info(r))
            return unavailable();
        if (!r.system_info.empty() && !out->ParseFromString(r.system_info)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed SystemInfo from supervisor");
        }
        // Warm the cache for next time (and ListMachines).
        if (!r.system_info.empty()) {
            std::lock_guard<std::mutex> lk(g_machines_mtx);
            g_machines[inst].system_info = r.system_info;
        }
        return grpc::Status::OK;
    }

    // Enumerate the cluster's machines from the scan-driven registry (instance,
    // name, present, cached host identity). The deterministic machine list the
    // GUI / `rtdb machines` use; names here are exactly what the Subscribe /
    // GetSystemInfo selectors accept.
    grpc::Status ListMachines(
            grpc::ServerContext*,
            const services::com::ListMachinesCall*,
            services::com::MachineList* out) override {
        // Snapshot the registry under lock, then build the reply outside it.
        std::vector<std::pair<uint32_t, MachineEntry>> snap;
        {
            std::lock_guard<std::mutex> lk(g_machines_mtx);
            snap.assign(g_machines.begin(), g_machines.end());
        }
        std::sort(snap.begin(), snap.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& kv : snap) {
            auto* mi = out->add_machines();
            mi->set_instance(kv.first);
            // Prefer the supervisor-reported name (kv.second.name), else manifest.
            mi->set_name(!kv.second.name.empty()
                             ? kv.second.name
                             : services_com::MachineManifest::instance()
                                   .name(kv.first));
            mi->set_present(kv.second.present);
            if (!kv.second.system_info.empty())
                mi->mutable_info()->ParseFromString(kv.second.system_info);
        }
        return grpc::Status::OK;
    }

    // Per-child log-level read-back — backs `rtdb loglevel` (no level arg).
    grpc::Status GetLogLevelConfig(
            grpc::ServerContext*,
            const services::com::GetLogLevelConfigCall*,
            system_supervisor::LogLevelConfigList* out) override {
        services_com::SupReply r;
        if (!services_com::SupLink::instance().get_log_level_config(r))
            return unavailable();
        if (!r.log_level_list.empty() &&
            !out->ParseFromString(r.log_level_list)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "malformed LogLevelConfigList from supervisor");
        }
        return grpc::Status::OK;
    }

    // Crash forensics — fetch a crashed child's tombstone bytes (capped at the
    // supervisor). The reply is com's OWN native message, so we copy SupReply
    // fields straight in (no proto-bytes round-trip). `found=false` is a valid,
    // non-error result (the child never crashed / no tombstone on disk).
    grpc::Status GetTombstone(
            grpc::ServerContext*,
            const services::com::GetTombstoneCall* req,
            services::com::GetTombstoneReply* out) override {
        std::string bare;
        auto& link = route_target(req->child_name(), bare);
        services_com::SupReply r;
        if (!link.get_tombstone(bare, r))
            return unavailable();
        out->set_found(r.tomb_found);
        out->set_path(r.tomb_path);
        out->set_truncated(r.tomb_truncated);
        out->set_total_bytes(r.tomb_total);
        out->set_content(r.tomb_content);
        return grpc::Status::OK;
    }

private:
    static grpc::Status unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "supervisor control link unavailable / timeout");
    }
    // Route a control op to the supervisor that OWNS the target. The aggregated
    // tree tags EVERY node "<machine_name>/<node>" (central/…, compute/…); a
    // control op on such a node must reach that machine's supervisor
    // (SupLink::for_instance(N)). Resolve the "<machine>/" prefix → instance via
    // the manifest's name→index lookup; strip it → bare name. An unprefixed
    // target (or an unknown machine) falls back to instance 0 (the local
    // supervisor). A real node name never contains '/'.
    static services_com::SupLink& route_target(const std::string& target,
                                               std::string& bare) {
        auto slash = target.find('/');
        if (slash != std::string::npos && slash >= 1) {
            const std::string machine = target.substr(0, slash);
            uint32_t inst = 0;
            if (services_com::MachineManifest::instance().index_of(machine, inst)) {
                bare = target.substr(slash + 1);
                auto& link = services_com::SupLink::for_instance(inst);
                // Lazily connect a peer link the first time a control op targets
                // it (tree fan-out may not have touched it yet).
                if (!link.connected()) link.start(/*timeout_ms=*/1500);
                return link;
            }
        }
        bare = target;
        return services_com::SupLink::instance();   // local (instance 0)
    }
    static void fill(::system_supervisor::ControlReply* out,
                     const services_com::SupReply& r) {
        out->set_status(r.status);
        out->set_message(r.message);
        out->set_child_name(r.child_name);
        if (!r.trace_config_list.empty())
            out->set_trace_config_list(r.trace_config_list);
    }
    grpc::Status name_op(uint32_t op_kind, const std::string& name,
                         ::system_supervisor::ControlReply* reply) {
        std::string bare;
        auto& link = route_target(name, bare);     // mN/ → that machine's sup
        services_com::SupReply r;
        if (!link.name_op(op_kind, bare, r))
            return unavailable();
        fill(reply, r);
        return grpc::Status::OK;
    }
    // No firehose member: Subscribe POLLS SupLink::get_tree() (the pull model).
};

// ---- gRPC service: PerView — proxy services/per's manager ops -------------
// com proxies per's schema-registry + snapshot ops over gRPC (so the GUI/rtdb
// inspect the config store without a second etcd client). PerLink RemoteRef-
// calls PerManager (TIPC 0x80010016); this edge translates per's primitive
// PerSchema/PerOpReply ↔ the libprotobuf PerView messages.
class PerViewImpl final : public services::com::PerView::Service {
public:
    grpc::Status ListSchemas(
            grpc::ServerContext*,
            const services::com::ListSchemasCall* req,
            services::com::PerSchemaList* out) override {
        std::vector<services_com::PerSchema> schemas;
        if (!services_com::PerLink::instance().list_schemas(
                req ? req->config_type() : "", schemas))
            return per_unavailable();
        for (const auto& s : schemas) {
            auto* row = out->add_schemas();
            row->set_config_type(s.config_type);
            row->set_digest(s.digest);
        }
        return grpc::Status::OK;
    }

    grpc::Status Snapshot(
            grpc::ServerContext*,
            const services::com::SnapshotCall* req,
            services::com::PerReply* out) override {
        services_com::PerOpReply r;
        if (!services_com::PerLink::instance().snapshot(
                req ? req->label() : "", r))
            return per_unavailable();
        out->set_status(r.status);
        out->set_message(r.message);
        out->set_mod_rev(r.mod_rev);
        return grpc::Status::OK;
    }

    grpc::Status GetSnapshot(
            grpc::ServerContext*,
            const services::com::GetSnapshotCall* req,
            services::com::PerStoreSnapshot* out) override {
        std::vector<services_com::PerStoreRow> rows;
        if (!services_com::PerLink::instance().get_snapshot(
                req ? req->config_type() : "", rows))
            return per_unavailable();
        for (const auto& r : rows) {
            auto* row = out->add_rows();
            row->set_config_type(r.config_type);
            row->set_digest(r.digest);
            row->set_config(r.config);   // RAW bytes — client decodes
        }
        return grpc::Status::OK;
    }

private:
    static grpc::Status per_unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "persistency link (per) unavailable / timeout");
    }
};

// ---- gRPC service: NmView — proxy services/nm's readiness + wifi -----------
// com proxies nm's GetNetworkStatus (the ladder) + WifiScan (visible APs) over
// gRPC so the GUI/rtdb get a wifi view without a TIPC client. NmLink RemoteRef-
// calls NmDaemon (TIPC 0x8001002E); this edge translates nm's primitive
// NmStatusInfo/NmWifiScanInfo ↔ the libprotobuf NmView messages.
class NmViewImpl final : public services::com::NmView::Service {
public:
    grpc::Status GetStatus(
            grpc::ServerContext*,
            const services::com::NmStatusCall*,
            services::com::NmStatus* out) override {
        services_com::NmStatusInfo s;
        if (!services_com::NmLink::instance().get_status(s))
            return nm_unavailable();
        out->set_state(s.state);
        out->set_interface(s.interface);
        out->set_has_carrier(s.has_carrier);
        out->set_has_address(s.has_address);
        out->set_vpn_up(s.vpn_up);
        out->set_ts_ns(s.ts_ns);
        return grpc::Status::OK;
    }

    grpc::Status WifiScan(
            grpc::ServerContext*,
            const services::com::WifiScanCall* req,
            services::com::NmWifiScan* out) override {
        services_com::NmWifiScanInfo r;
        if (!services_com::NmLink::instance().wifi_scan(
                req ? req->interface() : "", r))
            return nm_unavailable();
        out->set_interface(r.interface);
        out->set_associated(r.associated);
        out->set_assoc_ssid(r.assoc_ssid);
        out->set_assoc_bssid(r.assoc_bssid);
        for (const auto& b : r.bss) {
            auto* row = out->add_bss();
            row->set_ssid(b.ssid);
            row->set_bssid(b.bssid);
            row->set_signal_dbm(b.signal_dbm);
            row->set_freq_mhz(b.freq_mhz);
            row->set_security(b.security);
        }
        return grpc::Status::OK;
    }

    // ---- config-transaction write ops → NmCfgGate (via NmLink) -------------
    grpc::Status AddWifi(
            grpc::ServerContext*,
            const services::com::NmAddWifiCall* req,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().add_wifi(
                req->ssid(), req->psk(), req->priority(), r))
            return nm_unavailable();
        return fill(r, out);
    }

    grpc::Status RemoveWifi(
            grpc::ServerContext*,
            const services::com::NmRemoveWifiCall* req,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().remove_wifi(req->ssid(), r))
            return nm_unavailable();
        return fill(r, out);
    }

    grpc::Status SetVpn(
            grpc::ServerContext*,
            const services::com::NmSetVpnCall* req,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().set_vpn(
                req->require_vpn(), req->auto_vpn(), r))
            return nm_unavailable();
        return fill(r, out);
    }

    grpc::Status SetAutoConnect(
            grpc::ServerContext*,
            const services::com::NmSetAutoConnectCall* req,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().set_autoconnect(req->auto_connect(), r))
            return nm_unavailable();
        return fill(r, out);
    }

    grpc::Status ConfirmConfig(
            grpc::ServerContext*,
            const services::com::NmCfgEmpty*,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().confirm_config(r))
            return nm_unavailable();
        return fill(r, out);
    }

    grpc::Status AbortConfig(
            grpc::ServerContext*,
            const services::com::NmCfgEmpty*,
            services::com::NmCfgReply* out) override {
        services_com::NmCfgReplyInfo r;
        if (!services_com::NmLink::instance().abort_config(r))
            return nm_unavailable();
        return fill(r, out);
    }

private:
    static grpc::Status fill(const services_com::NmCfgReplyInfo& r,
                             services::com::NmCfgReply* out) {
        out->set_ok(r.ok);
        out->set_message(r.message);
        out->set_profiles(r.profiles);
        out->set_txn_state(r.txn_state);
        return grpc::Status::OK;
    }
    static grpc::Status nm_unavailable() {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "network-management link (nm) unavailable / timeout");
    }
};

// ---- gRPC service: DiagView — proxy services/diag's UDS router -------------
// com proxies one UDS request through the diag FC (read a DID, read DTCs) so the
// GUI / a tester runs diagnostics over com — no DoIP TCP / TIPC client. DiagLink
// RemoteRef-calls UdsRouter (TIPC 0x80010018); this edge translates the raw UDS
// bytes ↔ the libprotobuf DiagView messages.
class DiagViewImpl final : public services::com::DiagView::Service {
public:
    grpc::Status SendUds(
            grpc::ServerContext*,
            const services::com::DiagUdsRequest* req,
            services::com::DiagUdsReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        services_com::DiagUdsResult r =
            services_com::DiagLink::instance().send_uds(req->target_addr(), req->uds());
        if (!r.ok)
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "diagnostic link (diag) unavailable / timeout");
        out->set_uds(r.uds);
        out->set_is_nrc(r.is_nrc);
        out->set_ok(true);
        return grpc::Status::OK;
    }
};

// ---- gRPC service: UcmView — services/ucm (ara::ucm) over com gRPC ---------
// The ara::com edge an OTA client (GS / VUCM) uses to drive + observe the UCM
// agent. RequestUpdate forwards a package manifest to UcmDaemon (→ kicks the FSM:
// verify → SM-session → stop → install → activate → restart → PHM-verify →
// ACTIVE/ROLLBACK). GetProgress reads the latest UcmProgress the link folded off
// the PG group (the ECU-lifecycle plane GS shows beside the Mender plane).
class UcmViewImpl final : public services::com::UcmView::Service {
public:
    grpc::Status RequestUpdate(
            grpc::ServerContext*,
            const services::com::UcmRequestUpdateCall* req,
            services::com::UcmRequestUpdateReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        services_com::UcmUpdateReq r;
        r.name          = req->name();
        r.version       = req->version();
        r.kind          = req->kind();
        r.scope         = req->scope();
        r.artifact_path = req->artifact_path();
        r.signature     = req->signature();
        r.confirm_window_ms = req->confirm_window_ms();
        uint32_t status = 0;
        if (!services_com::UcmLink::instance().request_update(r, status))
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "ucm link unavailable / timeout");
        out->set_status(status);
        return grpc::Status::OK;
    }

    grpc::Status Confirm(
            grpc::ServerContext*,
            const services::com::UcmConfirmCall* req,
            services::com::UcmConfirmReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        uint32_t status = 0;
        if (!services_com::UcmLink::instance().confirm(
                req->campaign_id(), req->cancel(), status))
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "ucm link unavailable / timeout");
        out->set_status(status);
        return grpc::Status::OK;
    }

    grpc::Status GetProgress(
            grpc::ServerContext*,
            const services::com::UcmProgressCall*,
            services::com::UcmProgressReply* out) override {
        auto p = services_com::UcmLink::instance().latest_progress();
        out->set_state(p.state);
        out->set_version(p.version);
        out->set_kind(p.kind);
        out->set_scope(p.scope);
        out->set_detail(p.detail);
        out->set_ts_ns(p.ts_ns);
        out->set_ok(p.valid);
        // The SM-session plane, folded off the SmStateMsg group.
        auto sm = services_com::UcmLink::instance().latest_sm_state();
        out->set_sm_state(sm.state);
        out->set_sm_ts_ns(sm.ts_ns);
        out->set_sm_ok(sm.valid);
        return grpc::Status::OK;
    }
};

// ---- gRPC service: VucmView — services/vucm (the L4-B vehicle campaign) -----
// UcmView drives ONE board's installer; VucmView drives the VEHICLE campaign
// (V-UCM on the coordinator board): CheckForCampaign fans the package to every
// board's UCM + holds CMP_CONFIRMING until ALL are PROVISIONAL, then fans the
// aggregate Confirm. On a worker-only board (no vucm) the link stays unconnected
// and these RPCs return UNAVAILABLE.
class VucmViewImpl final : public services::com::VucmView::Service {
public:
    grpc::Status CheckForCampaign(
            grpc::ServerContext*,
            const services::com::VucmCampaignCall* req,
            services::com::VucmCampaignReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        services_com::VucmCampaignReq r;
        r.campaign_id = req->campaign_id();
        r.version     = req->version();
        r.scope       = req->scope();
        uint32_t accepted = 0, state = 0;
        if (!services_com::VucmLink::instance().check_for_campaign(r, accepted, state))
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "vucm link unavailable / timeout (no V-UCM on this board?)");
        out->set_accepted(accepted);
        out->set_state(state);
        return grpc::Status::OK;
    }

    grpc::Status GetCampaignStatus(
            grpc::ServerContext*,
            const services::com::VucmStatusCall*,
            services::com::VucmStatusReply* out) override {
        auto s = services_com::VucmLink::instance().status();
        out->set_state(s.state);
        out->set_campaign_id(s.campaign_id);
        out->set_version(s.version);
        out->set_detail(s.detail);
        out->set_ts_ns(s.ts_ns);
        out->set_valid(s.valid);
        return grpc::Status::OK;
    }

    grpc::Status Decide(
            grpc::ServerContext*,
            const services::com::VucmDecisionCall* req,
            services::com::VucmDecisionReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        uint32_t accepted = 0, state = 0;
        if (!services_com::VucmLink::instance().decide(
                req->campaign_id(), req->rollback(), accepted, state))
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "vucm link unavailable / timeout");
        out->set_accepted(accepted);
        out->set_state(state);
        return grpc::Status::OK;
    }
};

// ---- gRPC service: Provisioning — rig enrollment over com -----------------
// The centralized enrollment utility (colocated with Headscale) calls this to
// write the rig's identity (uuid/vin) + PKI creds under
// /etc/theia/manifest/<machine>/ — the theia-native analog of mosaic's
// cloud_provision (SSH → com here). Pure file I/O on the rig; no TIPC link.
class ProvisioningImpl final : public services::com::Provisioning::Service {
public:
    grpc::Status Provision(
            grpc::ServerContext*,
            const services::com::ProvisionRequest* req,
            services::com::ProvisionReply* out) override {
        if (!req) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "no request");
        const std::string machine = req->machine().empty()
            ? this_machine() : req->machine();
        const std::string base = manifest_dir(machine);
        const std::string certs = base + "/certs";

        if (!ensure_dir(base) || !ensure_dir(certs)) {
            out->set_ok(false);
            out->set_message("cannot create manifest dir " + base +
                             " (run com with write access / as the deploy user)");
            return grpc::Status::OK;
        }

        std::vector<std::string> written;
        auto put = [&](const std::string& path, const std::string& body,
                       bool secret) -> bool {
            if (body.empty()) return true;   // optional field omitted
            if (!write_file(path, body, secret)) {
                out->set_ok(false);
                out->set_message("write failed: " + path);
                return false;
            }
            written.push_back(path);
            return true;
        };

        if (!put(base + "/uuid", req->uuid(), false)) return grpc::Status::OK;
        if (!put(base + "/vin",  req->vin(),  false)) return grpc::Status::OK;
        if (!put(certs + "/client.key",    req->client_key(),  true))  return grpc::Status::OK;
        if (!put(certs + "/client.crt",    req->client_cert(), false)) return grpc::Status::OK;
        if (!put(certs + "/server_ca.crt", req->ca_chain(),    false)) return grpc::Status::OK;
        if (!put(certs + "/vpn.authkey",   req->vpn_authkey(), true))  return grpc::Status::OK;

        out->set_ok(true);
        out->set_message("provisioned " + machine + " (" +
                         std::to_string(written.size()) + " files)");
        for (const auto& w : written) out->add_written(w);
        std::fprintf(stderr, "[%s] provisioned machine=%s uuid=%s (%zu files)\n",
                     ComGrpcProxy::kNodeName, machine.c_str(),
                     req->uuid().c_str(), written.size());
        return grpc::Status::OK;
    }

    grpc::Status GetProvisionStatus(
            grpc::ServerContext*,
            const services::com::ProvisionStatusRequest*,
            services::com::ProvisionStatusReply* out) override {
        const std::string machine = this_machine();
        const std::string base = manifest_dir(machine);
        const std::string certs = base + "/certs";
        out->set_machine(machine);
        out->set_uuid(read_file(base + "/uuid"));
        out->set_vin(read_file(base + "/vin"));
        out->set_has_client_key(exists(certs + "/client.key"));
        out->set_has_client_cert(exists(certs + "/client.crt"));
        out->set_has_ca_chain(exists(certs + "/server_ca.crt"));
        out->set_has_vpn_authkey(exists(certs + "/vpn.authkey"));
        return grpc::Status::OK;
    }

private:
    static std::string this_machine() {
        const char* m = std::getenv("THEIA_MACHINE");
        return m && *m ? m : "central";
    }
    static std::string manifest_dir(const std::string& machine) {
        const char* root = std::getenv("THEIA_MACHINE_MANIFEST");
        std::string base = (root && *root) ? root : "/etc/theia/manifest";
        return base + "/" + machine;
    }
    static bool ensure_dir(const std::string& d) {
        if (::mkdir(d.c_str(), 0755) == 0) return true;
        return errno == EEXIST;
    }
    static bool write_file(const std::string& path, const std::string& body,
                           bool secret) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.close();
        if (!f) return false;
        ::chmod(path.c_str(), secret ? 0600 : 0644);
        return true;
    }
    static std::string read_file(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "";
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }
    static bool exists(const std::string& path) {
        struct stat st{};
        return ::stat(path.c_str(), &st) == 0;
    }
};

// ---- File-static runnable state (one ComGrpcProxy per process) -----------
// Held here rather than as ComGrpcProxy members so lib/ComGrpcProxy.hh stays
// byte-stable against gen-app. do_start builds them; do_stop tears them down.
// No firehose uplink: the live tree is a GetTree poll (SupLink), so com needs
// no ComDaemon cast-sink wiring — Subscribe pulls on an interval.
std::unique_ptr<SupervisorViewImpl>         g_sup_svc;
std::unique_ptr<PerViewImpl>                g_per_svc;
std::unique_ptr<NmViewImpl>                 g_nm_svc;
std::unique_ptr<DiagViewImpl>               g_diag_svc;
std::unique_ptr<UcmViewImpl>                g_ucm_svc;
std::unique_ptr<VucmViewImpl>               g_vucm_svc;
std::unique_ptr<ProvisioningImpl>           g_prov_svc;
std::unique_ptr<grpc::Server>               g_server;
std::atomic<bool>                           g_up{false};

}  // namespace

// One-time setup on the worker thread, before do_loop(): open the supervisor
// TIPC uplink, then build + start the gRPC SupervisorView server. If either
// fails we leave g_up false so do_loop() falls straight through and the
// runnable exits — the supervisor's watchdog then restarts com.
void ComGrpcProxy::do_start() {
    std::fprintf(stderr, "[%s] runnable starting\n", kNodeName);

    // #418 — control link over the standard transport (RemoteRef → SupervisorCtl
    // at 0x80020001/0). Best-effort: if the supervisor isn't reachable yet,
    // control RPCs (and the Subscribe GetTree poll) return UNAVAILABLE until a
    // restart. We do NOT hard-fail do_start on it.
    if (!services_com::SupLink::instance().start()) {
        std::fprintf(stderr,
                     "[%s] WARN: supervisor control link (RemoteRef "
                     "0x80020001/0) not reachable; control RPCs will return "
                     "UNAVAILABLE until it connects\n", kNodeName);
    } else {
        std::fprintf(stderr,
                     "[%s] supervisor control link up (RemoteRef 0x80020001/0)\n",
                     kNodeName);
    }

    // Stage 3 — live discovery of which machines' supervisors are present.
    // Subscribe to all instances [0..kMaxSupInstance] of the supervisor control
    // type; the Subscribe poll fans get_tree() across instances_of(). CLUSTER
    // scope, so com on central sees compute's supervisor. Best-effort: if TIPC's
    // topology server is unreachable we fall back to instance-0-only (g_topology_up
    // stays false → merge_present_trees still always tries instance 0).
    g_sup_topology.subscribe(kSupCtlTipcType, 0u, kMaxSupInstance);
    if (g_sup_topology.start([](const theia::runtime::TopologyEvent& ev) {
            std::fprintf(stderr,
                         "[com] supervisor instance %u %s\n",
                         ev.instance, ev.present ? "PUBLISHED" : "WITHDRAWN");
            // Maintain the machine registry off the cluster scan: mark presence,
            // and on a fresh PUBLISH fetch+cache that supervisor's host identity
            // (GetSystemInfo) on a DETACHED thread so a slow peer never stalls
            // this callback. WITHDRAWN keeps the cached identity (present=false).
            set_machine_present(ev.instance, ev.present);
            if (ev.present)
                std::thread(cache_machine_sysinfo, ev.instance).detach();
        })) {
        // Instance 0 (the local supervisor) is always "present" and may have
        // PUBLISHED before this callback was wired — seed it explicitly so the
        // single-machine stack lists central immediately.
        set_machine_present(0u, true);
        std::thread(cache_machine_sysinfo, 0u).detach();
        g_topology_up.store(true);
        std::fprintf(stderr,
                     "[%s] supervisor-instance topology up "
                     "(0x80020001, 0..%u); tree fans out per machine\n",
                     kNodeName, kMaxSupInstance);
    } else {
        std::fprintf(stderr,
                     "[%s] WARN: TIPC topology server unreachable; tree stays "
                     "single-machine (instance 0 only)\n", kNodeName);
    }

    // per (persistency) proxy link — RemoteRef → PerManager (0x80010016).
    // Best-effort + OFF the do_start critical path: PerLink::start() does a
    // blocking TIPC connect (up to its retry budget, and a TIPC connect to a
    // not-yet-accepting peer can stall longer), which would HOLD UP the :7700
    // gRPC bind below — the whole SupervisorView/PerView server. per is optional
    // (down/absent on a machine, or slow to bind), so connect it on a DETACHED
    // thread; PerView RPCs return UNAVAILABLE until it links. The first
    // PerView call after it links works; no com restart needed.
    std::thread([] {
        if (services_com::PerLink::instance().start())
            std::fprintf(stderr,
                         "[%s] persistency link up (RemoteRef 0x80010016/0)\n",
                         kNodeName);
        else
            std::fprintf(stderr,
                         "[%s] WARN: persistency link (per 0x80010016) not "
                         "reachable; PerView RPCs return UNAVAILABLE\n",
                         kNodeName);
    }).detach();

    // Same detached-link treatment for nm (network management): NmView RPCs
    // return UNAVAILABLE until NmDaemon (0x8001002E) links. Optional/absent on a
    // machine with no NM; the gRPC bind must not block on it.
    std::thread([] {
        if (services_com::NmLink::instance().start())
            std::fprintf(stderr,
                         "[%s] network-mgmt link up (RemoteRef 0x8001002E/0)\n",
                         kNodeName);
        else
            std::fprintf(stderr,
                         "[%s] WARN: network-mgmt link (nm 0x8001002E) not "
                         "reachable; NmView RPCs return UNAVAILABLE\n",
                         kNodeName);
    }).detach();

    // Same detached-link treatment for diag (DoIP/UDS): DiagView.SendUds returns
    // UNAVAILABLE until UdsRouter (0x80010018) links. diag is optional/absent on
    // many machines; the gRPC bind must not block on it.
    std::thread([] {
        if (services_com::DiagLink::instance().start())
            std::fprintf(stderr,
                         "[%s] diagnostic link up (RemoteRef 0x80010018/0)\n",
                         kNodeName);
        else
            std::fprintf(stderr,
                         "[%s] WARN: diagnostic link (diag 0x80010018) not "
                         "reachable; DiagView RPCs return UNAVAILABLE\n",
                         kNodeName);
    }).detach();

    // Same detached-link treatment for ucm (ara::ucm OTA agent): UcmView.RequestUpdate
    // returns UNAVAILABLE until UcmDaemon (0x8001000E) links; GetProgress reads the
    // UcmProgress PG group. The ara::com edge for GS/VUCM-driven installs.
    std::thread([] {
        if (services_com::UcmLink::instance().start())
            std::fprintf(stderr,
                         "[%s] ucm link up (RemoteRef 0x8001000E/0 + UcmProgress pg)\n",
                         kNodeName);
        else
            std::fprintf(stderr,
                         "[%s] WARN: ucm link (ucm 0x8001000E) not reachable; "
                         "UcmView RPCs return UNAVAILABLE\n", kNodeName);
    }).detach();

    // vucm (L4-B vehicle campaign) — only the COORDINATOR board runs V-UCM, so a
    // worker-only board's link simply stays down (VucmView RPCs → UNAVAILABLE).
    std::thread([] {
        if (services_com::VucmLink::instance().start())
            std::fprintf(stderr,
                         "[%s] vucm link up (RemoteRef 0x8001005E/0 — VucmView)\n",
                         kNodeName);
        else
            std::fprintf(stderr,
                         "[%s] vucm link (vucm 0x8001005E) not reachable "
                         "(worker board?); VucmView RPCs return UNAVAILABLE\n", kNodeName);
    }).detach();

    // Load the cluster machine manifest (index→name) once, up front, so the
    // first AccelSample's machine_name lookup is warm and the load is logged at
    // startup (vs lazily on the recv thread). Best-effort: no manifest → "mN".
    (void)services_com::MachineManifest::instance();

    // SHWA AccelTelemetry egress receiver. Bind the egress DGRAM name + start the
    // recv thread; install the gated sink BEFORE start() so no sample is missed.
    // Best-effort: a quiet/absent SHWA just means no `accel` observations.
    services_com::ShwaLink::instance().set_sink(
        [](const std::string& bytes) { on_accel_sample(bytes); });
    if (!services_com::ShwaLink::instance().start()) {
        std::fprintf(stderr,
                     "[%s] WARN: SHWA AccelTelemetry egress bind failed; "
                     "GPU/host panels will get no feed\n", kNodeName);
    } else {
        std::fprintf(stderr,
                     "[%s] SHWA AccelTelemetry egress receiver up (0x8001001A/0)\n",
                     kNodeName);
    }

    g_sup_svc  = std::make_unique<SupervisorViewImpl>();
    g_per_svc  = std::make_unique<PerViewImpl>();
    g_nm_svc   = std::make_unique<NmViewImpl>();
    g_diag_svc = std::make_unique<DiagViewImpl>();
    g_ucm_svc  = std::make_unique<UcmViewImpl>();
    g_vucm_svc = std::make_unique<VucmViewImpl>();
    g_prov_svc = std::make_unique<ProvisioningImpl>();

    const std::string listen = listen_addr();
    // Bind with a short bounded retry. On a supervisor restart / OTA FC-swap the
    // previous com may not have released the listen port yet (TIME_WAIT / a slow
    // teardown) — BuildAndStart() then returns null with EADDRINUSE. Giving up
    // immediately leaves a live TIPC node with no gRPC edge (the operator/rtdb
    // path is dead until a manual sweep). Retry a few times so the transient
    // window self-heals; only escalate if the port is genuinely held.
    constexpr int kBindAttempts = 10;
    constexpr int kBindBackoffMs = 500;
    for (int attempt = 1; attempt <= kBindAttempts && !g_server; ++attempt) {
        grpc::ServerBuilder b;
        b.AddListeningPort(listen, services_com::make_server_creds(kNodeName));
        b.RegisterService(g_sup_svc.get());
        b.RegisterService(g_per_svc.get());
        b.RegisterService(g_nm_svc.get());
        b.RegisterService(g_diag_svc.get());
        b.RegisterService(g_ucm_svc.get());
        b.RegisterService(g_vucm_svc.get());
        b.RegisterService(g_prov_svc.get());
        g_server = b.BuildAndStart();
        if (!g_server && attempt < kBindAttempts) {
            std::fprintf(stderr,
                         "[%s] gRPC bind on %s busy (attempt %d/%d) — retrying in "
                         "%dms\n", kNodeName, listen.c_str(), attempt,
                         kBindAttempts, kBindBackoffMs);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kBindBackoffMs));
        }
    }
    if (!g_server) {
        std::fprintf(stderr, "[%s] gRPC server failed to start on %s\n",
                     kNodeName, listen.c_str());
        g_sup_svc.reset();
        g_per_svc.reset();
        g_nm_svc.reset();
        g_diag_svc.reset();
        g_ucm_svc.reset();
        g_prov_svc.reset();
        services_com::SupLink::instance().stop();
        services_com::PerLink::instance().stop();
        services_com::NmLink::instance().stop();
        services_com::DiagLink::instance().stop();
        services_com::UcmLink::instance().stop();
        services_com::ShwaLink::instance().stop();
        return;
    }
    std::fprintf(stderr,
                 "[%s] gRPC SupervisorView + PerView + NmView + DiagView + "
                 "Provisioning listening on %s\n", kNodeName, listen.c_str());
    g_up.store(true);
}

// The body. grpc::Server runs its own thread pool from BuildAndStart(), so
// this loop just parks until stop_requested() (do_stop wakes the server). A
// non-reporting runnable (kReporting==false), so no watchdog beat is needed —
// a plain cooperative poll is correct here.
void ComGrpcProxy::do_loop() {
    if (!g_up.load()) {
        std::fprintf(stderr, "[%s] startup failed; loop exiting immediately\n",
                     kNodeName);
        return;
    }

    // PHM health edge (escalation model): com's supervisor control uplink (SupLink
    // → SupervisorCtl) is its critical dependency — if it can't reach the
    // supervisor, the whole remote-management plane (GUI/rtdb control RPCs + the
    // tree Subscribe) is degraded. Report a health INDICATION to PHM on the
    // reachable↔unreachable EDGE only (com is reporting=false: no watchdog
    // heartbeat, so this PG cast is its sole health signal). PHM aggregates → SM.
    // Pure PG broadcaster (the runnable pump pattern — no generated broadcast_*
    // for a runnable sender); group resolved lazily (supervisor may be late).
    using FcHealthReport = system_services_phm_FcHealthReport;
    ::theia::runtime::PgClient phm_pg;
    phm_pg.attach(kNodeName, /*binding=*/nullptr);
    uint32_t phm_group = 0;
    int last_up = -1;   // -1 = unreported; 0 = down, 1 = up — edge latch

    auto report_health = [&](bool up) {
        if (phm_group == 0) {
            auto g = phm_pg.resolve<FcHealthReport>();
            if (!g.ok) return;          // supervisor/allocator down — drop (retry next edge)
            phm_group = g.type;
        }
        FcHealthReport hr = system_services_phm_FcHealthReport_init_zero;
        std::snprintf(hr.entity, sizeof(hr.entity), "%s", kNodeName);
        hr.fg = 1;                       // FG_PLATFORM (com ∈ core_sup)
        if (up) {
            hr.level = system_services_phm_HealthLevel_HealthLevel_OK;
            hr.code  = 0;
            std::snprintf(hr.detail, sizeof(hr.detail), "supervisor uplink up");
        } else {
            hr.level = system_services_phm_HealthLevel_HealthLevel_DEGRADED;
            hr.code  = 1;
            std::snprintf(hr.detail, sizeof(hr.detail), "supervisor uplink unreachable");
        }
        uint8_t buf[256];
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&os, ::theia::runtime::RemoteCodec<FcHealthReport>::fields(), &hr))
            return;
        phm_pg.broadcast<FcHealthReport>(phm_group, buf,
                                         static_cast<uint16_t>(os.bytes_written));
    };

    while (!stop_requested()) {
        const int up = services_com::SupLink::instance().connected() ? 1 : 0;
        if (up != last_up) {             // edge only — never per-tick keep-alive
            report_health(up == 1);
            last_up = up;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "[%s] runnable loop exiting\n", kNodeName);
}

// Release + signal do_loop() to return: drain the gRPC server (bounded
// deadline so in-flight RPCs finish), then close the supervisor uplink.
void ComGrpcProxy::do_stop() {
    std::fprintf(stderr, "[%s] runnable stopping\n", kNodeName);
    if (g_server) {
        g_server->Shutdown(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(500));
        g_server.reset();
    }
    g_sup_svc.reset();
    g_per_svc.reset();
    g_nm_svc.reset();
    g_diag_svc.reset();
    g_ucm_svc.reset();
    g_prov_svc.reset();
    g_sup_topology.stop();
    g_topology_up.store(false);
    services_com::SupLink::instance().stop();
    services_com::PerLink::instance().stop();
    services_com::NmLink::instance().stop();
    services_com::DiagLink::instance().stop();
    services_com::UcmLink::instance().stop();
    services_com::ShwaLink::instance().stop();
    g_up.store(false);
}

}  // namespace ara::com
