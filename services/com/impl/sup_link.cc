// sup_link implementation — RemoteRef + reply-pump TipcMux to the supervisor
// control node. See sup_link.hpp.
//
// This is the ONLY com TU that touches the nanopb supervisor control structs
// (system_supervisor_ControlRequest/Reply). The gRPC edge stays on
// libprotobuf; we translate to/from primitives at this boundary so the
// same-basename ControlRequest.pb.h never meets the libprotobuf one.

#include "impl/sup_link.hpp"

// Standard transport + the supervisor control codecs. supervisor_codecs.hh
// brings RemoteCodec<system_supervisor_ControlRequest/Reply> so RemoteRef
// dispatches by the same service_id the supervisor's register_call uses.
#include "NodeRef.hh"
#include "TipcMux.hh"
#include "system/supervisor/supervisor.pb.h"   // nanopb (consolidated)
#include "lib/supervisor_codecs.hh"      // RemoteCodec specializations
#include <pb_encode.h>                   // GetTraceConfig re-encode

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// Control address — the gen-app supervisor's SupervisorCtl node (system.art:
// `node atomic SupervisorCtl { tipc type=0x80020001 instance=0 }`). The current
// supervisor UNIFIED control + the event/health/firehose on this one address
// (the old split-out 0x80020003 control node is gone). This is exactly what
// tdb's probe resolves SupervisorCtl to. Instance is per-machine (central=0,
// compute=1) — see sup_instance below.
constexpr uint32_t kSupCtlTipcType     = 0x80020001u;
constexpr uint32_t kSupCtlTipcInstance = 0u;

// RemoteRef needs a NodeType only for the trace tag (NodeType::kNodeName) and
// to name the destination. This is the peer's identity from com's side.
struct SupervisorCtlTag {
    static constexpr const char* kNodeName = "supervisor_ctl";
};

using SupRef = theia::runtime::RemoteRef<SupervisorCtlTag,
                                        kSupCtlTipcType, kSupCtlTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}

// Log-level name → LogLevelValue ordinal (the same map tdb uses). Defaults to
// info (2) on an unknown name.
uint32_t level_ordinal(const std::string& level) {
    if (level == "trace") return 0;
    if (level == "debug") return 1;
    if (level == "info")  return 2;
    if (level == "warn")  return 3;
    if (level == "error") return 4;
    return 2;
}

}  // namespace

struct SupLink::Impl {
    theia::runtime::TipcMux  mux;        // reply pump for ref's client fd
    SupRef                  ref;
    bool                    started = false;
    std::mutex              call_mu;    // serialize call(); RemoteRef demuxes
                                        // by corr_id, but one outstanding call
                                        // at a time keeps the contract simple.
    std::atomic<uint32_t>   corr{1};

    // The current supervisor exposes PER-OP typed request messages (each a
    // distinct service_id via register_call<Req, Rep>), called by name — the
    // SAME surface tdb's probe calls. So each op builds its specific nanopb
    // request and RemoteRef-calls it with the matching reply type. (The old
    // single ControlRequest{op_kind} envelope is gone.)
    //
    // ControlReply ops → fill SupReply{status,message,child_name}.
    template <typename Req>
    bool call_reply(const Req& req, SupReply& out, int timeout_ms) {
        auto result = theia::runtime::call<system_supervisor_ControlReply>(
            ref, req, /*act=*/0, timeout_ms);
        if (result.tag != theia::runtime::CallTag::Reply) return false;
        const auto& rep = result.reply;
        out.status     = rep.status;
        out.message    = rep.message;
        out.child_name = rep.child_name;
        return true;
    }
};

SupLink::SupLink() : impl_(new Impl()) {}
SupLink::~SupLink() { stop(); delete impl_; }

SupLink& SupLink::instance() {
    static SupLink s;
    return s;
}

bool SupLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    if (!impl_->ref.connect(connect_timeout_ms)) {
        return false;   // supervisor not up / TIPC unavailable
    }
    // Watch the ref's client fd for GW_MSG_GEN_CALL_REPLY frames so call()'s
    // future is fulfilled. The mux's epoll thread pumps replies.
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void SupLink::stop() {
    if (!impl_->started) return;
    impl_->mux.stop();
    impl_->started = false;
}

bool SupLink::connected() const { return impl_->started; }

uint32_t SupLink::next_correlation_id() { return impl_->corr++; }

// ---- op-specific forwards -------------------------------------------------

bool SupLink::start_child(const SupChildSpec& spec, SupReply& out,
                          int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_StartChildRequest req =
        system_supervisor_StartChildRequest_init_zero;
    req.has_spec = true;
    auto& sp = req.spec;
    set_str(sp.name, sizeof(sp.name), spec.name);
    set_str(sp.parent_supervisor, sizeof(sp.parent_supervisor),
            spec.parent_supervisor);
    sp.restart  = spec.restart;
    sp.shutdown = spec.shutdown;
    sp.type     = spec.type;
    sp.start_cmd_count = 0;
    for (const auto& arg : spec.start_cmd) {
        if (sp.start_cmd_count >=
            (pb_size_t)(sizeof(sp.start_cmd) / sizeof(sp.start_cmd[0]))) break;
        set_str(sp.start_cmd[sp.start_cmd_count],
                sizeof(sp.start_cmd[0]), arg);
        ++sp.start_cmd_count;
    }
    sp.modules_count = 0;
    for (const auto& m : spec.modules) {
        if (sp.modules_count >=
            (pb_size_t)(sizeof(sp.modules) / sizeof(sp.modules[0]))) break;
        set_str(sp.modules[sp.modules_count], sizeof(sp.modules[0]), m);
        ++sp.modules_count;
    }
    return impl_->call_reply(req, out, timeout_ms);
}

bool SupLink::name_op(uint32_t op_kind, const std::string& name, SupReply& out,
                      int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    // Delete has its own request; Restart + Terminate share ChildSelector
    // (no_restart distinguishes terminate-and-hold from restart).
    if (op_kind == kSupOpDeleteChild) {
        system_supervisor_DeleteChildRequest req =
            system_supervisor_DeleteChildRequest_init_zero;
        set_str(req.name, sizeof(req.name), name);
        return impl_->call_reply(req, out, timeout_ms);
    }
    system_supervisor_ChildSelector req =
        system_supervisor_ChildSelector_init_zero;
    set_str(req.name, sizeof(req.name), name);
    req.no_restart = (op_kind == kSupOpTerminateChild);
    return impl_->call_reply(req, out, timeout_ms);
}

bool SupLink::stop_supervisor(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_Stop req = system_supervisor_Stop_init_zero;
    return impl_->call_reply(req, out, timeout_ms);
}

bool SupLink::configure_log_level(const std::string& target_node,
                                  const std::string& level, SupReply& out,
                                  int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_ConfigureLogLevelRequest req =
        system_supervisor_ConfigureLogLevelRequest_init_zero;
    req.has_config = true;
    auto& cfg = req.config;
    set_str(cfg.target_node, sizeof(cfg.target_node), target_node);
    // level name → LogLevelValue ordinal (same map tdb uses).
    cfg.has_log_level = true;
    cfg.log_level.level =
        (platform_runtime_LogLevelValue)level_ordinal(level);
    return impl_->call_reply(req, out, timeout_ms);
}

bool SupLink::configure_trace(const std::string& target_node,
                              const std::string& /*msg_type*/, bool enabled,
                              uint32_t kind, SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_ConfigureTraceRequest req =
        system_supervisor_ConfigureTraceRequest_init_zero;
    req.has_config = true;
    auto& cfg = req.config;
    set_str(cfg.target_node, sizeof(cfg.target_node), target_node);
    // (msg_type is no longer carried — trace is a per-node kind bitmask.)
    cfg.has_trace_ctrl = true;
    cfg.trace_ctrl.kind    = (platform_runtime_TraceKind)kind;
    cfg.trace_ctrl.enabled = enabled;
    return impl_->call_reply(req, out, timeout_ms);
}

bool SupLink::get_trace_config(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_GetTraceConfigRequest req =
        system_supervisor_GetTraceConfigRequest_init_zero;
    // Reply is a TraceConfigList (not ControlReply): re-serialize it to raw
    // proto bytes for the gRPC read-back (SupReply.trace_config_list).
    auto result = theia::runtime::call<system_supervisor_TraceConfigList>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    // Encode the nanopb TraceConfigList back to bytes (wire-identical to the
    // libprotobuf encoding the gRPC client decodes).
    uint8_t buf[4096];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os,
            theia::runtime::RemoteCodec<system_supervisor_TraceConfigList>::fields(),
            &result.reply)) {
        out.trace_config_list.assign(reinterpret_cast<const char*>(buf),
                                     os.bytes_written);
    }
    out.status = 0;
    return true;
}

bool SupLink::get_tree(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_GetTreeRequest req =
        system_supervisor_GetTreeRequest_init_zero;
    // Reply is a TreeSnapshot (not ControlReply). Re-serialize it to raw proto
    // bytes for the gRPC Subscribe stream (SupReply.tree_snapshot), wire-
    // identical to the libprotobuf TreeSnapshot the gRPC client decodes.
    auto result = theia::runtime::call<system_supervisor_TreeSnapshot>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    // TreeSnapshot.children is max_count:64, each ChildState up to ~360B →
    // ~23KB worst case. 48KB matches the runtime's bumped reply ceiling.
    static uint8_t buf[48 * 1024];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os,
            theia::runtime::RemoteCodec<system_supervisor_TreeSnapshot>::fields(),
            &result.reply)) {
        out.tree_snapshot.assign(reinterpret_cast<const char*>(buf),
                                 os.bytes_written);
    }
    out.status = 0;
    return true;
}

bool SupLink::get_system_info(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_GetSystemInfoRequest req =
        system_supervisor_GetSystemInfoRequest_init_zero;
    auto result = theia::runtime::call<system_supervisor_SystemInfo>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    uint8_t buf[2048];   // SystemInfo: ~9 fields, longest str max_size:128
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os,
            theia::runtime::RemoteCodec<system_supervisor_SystemInfo>::fields(),
            &result.reply)) {
        out.system_info.assign(reinterpret_cast<const char*>(buf),
                               os.bytes_written);
    }
    out.status = 0;
    return true;
}

bool SupLink::get_log_level_config(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_GetLogLevelConfigRequest req =
        system_supervisor_GetLogLevelConfigRequest_init_zero;
    auto result = theia::runtime::call<system_supervisor_LogLevelConfigList>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    // LogLevelConfigList.configs is max_count:64 × ~80B ≈ 5KB; 8KB is safe.
    static uint8_t buf[8 * 1024];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os,
            theia::runtime::RemoteCodec<system_supervisor_LogLevelConfigList>::fields(),
            &result.reply)) {
        out.log_level_list.assign(reinterpret_cast<const char*>(buf),
                                  os.bytes_written);
    }
    out.status = 0;
    return true;
}

bool SupLink::get_tombstone(const std::string& child_name, SupReply& out,
                            int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_supervisor_GetTombstoneRequest req =
        system_supervisor_GetTombstoneRequest_init_zero;
    set_str(req.child_name, sizeof(req.child_name), child_name);
    auto result = theia::runtime::call<system_supervisor_GetTombstoneReply>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    // Reply IS the tombstone (not a ControlReply): copy the fields straight
    // into SupReply — com re-exposes them as native gRPC fields, no re-encode.
    const auto& r = result.reply;
    out.tomb_found     = r.found;
    out.tomb_truncated = r.truncated;
    out.tomb_total     = r.total_bytes;
    out.tomb_path.assign(r.path);
    out.tomb_content.assign(reinterpret_cast<const char*>(r.content.bytes),
                            r.content.size);
    out.status = 0;
    return true;
}

}  // namespace services_com
