// sup_link implementation — RemoteRef + reply-pump TipcMux to the supervisor
// control node. See sup_link.hpp.
//
// This is the ONLY com TU that touches the nanopb supervisor control structs
// (services_supervisor_ControlRequest/Reply). The gRPC edge stays on
// libprotobuf; we translate to/from primitives at this boundary so the
// same-basename ControlRequest.pb.h never meets the libprotobuf one.

#include "impl/sup_link.hpp"

// Standard transport + the supervisor control codecs. supervisor_codecs.hh
// brings RemoteCodec<services_supervisor_ControlRequest/Reply> so RemoteRef
// dispatches by the same service_id the supervisor's register_call uses.
#include "NodeRef.hh"
#include "TipcMux.hh"
#include "supervisor/supervisor_codecs.hh"

#include "ControlRequest.pb.h"   // nanopb
#include "ControlReply.pb.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// Control address — MUST match SupervisorControlNode::kTipcType/Instance in
// platform/supervisor/include/supervisor/control_node.h (the distinct control
// type, NOT the publisher's 0x80020001).
constexpr uint32_t kSupCtlTipcType     = 0x80020003u;
constexpr uint32_t kSupCtlTipcInstance = 0u;

// RemoteRef needs a NodeType only for the trace tag (NodeType::kNodeName) and
// to name the destination. This is the peer's identity from com's side.
struct SupervisorCtlTag {
    static constexpr const char* kNodeName = "supervisor_ctl";
};

using SupRef = demo::runtime::RemoteRef<SupervisorCtlTag,
                                        kSupCtlTipcType, kSupCtlTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}

}  // namespace

struct SupLink::Impl {
    demo::runtime::TipcMux  mux;        // reply pump for ref's client fd
    SupRef                  ref;
    bool                    started = false;
    std::mutex              call_mu;    // serialize call(); RemoteRef demuxes
                                        // by corr_id, but one outstanding call
                                        // at a time keeps the contract simple.
    std::atomic<uint32_t>   corr{1};

    // Encode req, RemoteRef-call, decode reply into out. Caller holds call_mu.
    bool do_call(const services_supervisor_ControlRequest& req,
                 SupReply& out, int timeout_ms) {
        auto result =
            demo::runtime::call<services_supervisor_ControlReply>(
                ref, req, /*act=*/0, timeout_ms);
        if (result.tag != demo::runtime::CallTag::Reply) return false;
        const auto& rep = result.reply;
        out.status     = rep.status;
        out.message    = rep.message;
        out.child_name = rep.child_name;
        out.trace_config_list.assign(
            reinterpret_cast<const char*>(rep.trace_config_list.bytes),
            rep.trace_config_list.size);
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
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = kSupOpStartChild;
    req.correlation_id = next_correlation_id();
    req.has_start_child = true;
    auto& s = req.start_child;
    s.has_spec = true;
    auto& sp = s.spec;
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
    return impl_->do_call(req, out, timeout_ms);
}

bool SupLink::name_op(uint32_t op_kind, const std::string& name, SupReply& out,
                      int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = op_kind;
    req.correlation_id = next_correlation_id();
    switch (op_kind) {
        case kSupOpDeleteChild:
            req.has_delete_child = true;
            set_str(req.delete_child.name, sizeof(req.delete_child.name), name);
            break;
        case kSupOpRestartChild:
            req.has_restart_child = true;
            set_str(req.restart_child.name, sizeof(req.restart_child.name),
                    name);
            break;
        case kSupOpTerminateChild:
            req.has_terminate_child = true;
            set_str(req.terminate_child.name, sizeof(req.terminate_child.name),
                    name);
            break;
        default:
            return false;
    }
    return impl_->do_call(req, out, timeout_ms);
}

bool SupLink::stop_supervisor(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = kSupOpStop;
    req.correlation_id = next_correlation_id();
    return impl_->do_call(req, out, timeout_ms);
}

bool SupLink::configure_log_level(const std::string& target_node,
                                  const std::string& level, SupReply& out,
                                  int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = kSupOpConfigureLogLevel;
    req.correlation_id = next_correlation_id();
    req.has_configure_log_level = true;
    req.configure_log_level.has_config = true;
    auto& cfg = req.configure_log_level.config;
    set_str(cfg.target_node, sizeof(cfg.target_node), target_node);
    set_str(cfg.level, sizeof(cfg.level), level);
    return impl_->do_call(req, out, timeout_ms);
}

bool SupLink::configure_trace(const std::string& target_node,
                              const std::string& msg_type, bool enabled,
                              uint32_t kind, SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = kSupOpConfigureTrace;
    req.correlation_id = next_correlation_id();
    req.has_configure_trace = true;
    req.configure_trace.has_config = true;
    auto& cfg = req.configure_trace.config;
    set_str(cfg.target_node, sizeof(cfg.target_node), target_node);
    set_str(cfg.msg_type, sizeof(cfg.msg_type), msg_type);
    cfg.enabled = enabled;
    cfg.kind    = kind;
    return impl_->do_call(req, out, timeout_ms);
}

bool SupLink::get_trace_config(SupReply& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    services_supervisor_ControlRequest req =
        services_supervisor_ControlRequest_init_zero;
    req.op_kind        = kSupOpGetTraceConfig;
    req.correlation_id = next_correlation_id();
    return impl_->do_call(req, out, timeout_ms);
}

}  // namespace services_com
