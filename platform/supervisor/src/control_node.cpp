// SupervisorControlNode::handle_call + Supervisor::dispatch_control_nanopb.
//
// The control surface on the standard Theia transport. handle_call receives a
// nanopb ControlRequest (decoded by TipcMux from a GW_MSG_GEN_CALL frame) and
// returns a nanopb ControlReply (TipcMux encodes it into GW_MSG_GEN_CALL_REPLY).
// The dispatch thunks into the existing orchestrator primitives (do_*/apply_*)
// — same logic as the legacy on_inbound_frame ControlRequest switch, but on
// nanopb structs and only for the single-reply ops. See
// docs/com-supervisor-transport.md §4-5.

#include "supervisor/control_node.h"
#include "supervisor/control_server.h"
#include "supervisor/runtime.h"

#include "ControlRequest.pb.h"   // nanopb
#include "ControlReply.pb.h"
#include "TraceConfigList.pb.h"
#include "TraceConfig.pb.h"

// Standard-transport plumbing. supervisor_codecs.hh brings the RemoteCodec
// specializations so register_call<> below dispatches by service_id.
#include "supervisor/supervisor_codecs.hh"
#include "TipcMux.hh"

#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace supervisor {

namespace {
// op_kind values — shared with the .art ControlRequest envelope + the legacy
// on_inbound_frame switch in runtime.cpp.
constexpr uint32_t kOpStartChild       = 3;
constexpr uint32_t kOpDeleteChild      = 4;
constexpr uint32_t kOpRestartChild     = 5;
constexpr uint32_t kOpTerminateChild   = 6;
constexpr uint32_t kOpStop             = 7;
constexpr uint32_t kOpConfigureTrace   = 9;
constexpr uint32_t kOpGetTraceConfig   = 10;
constexpr uint32_t kOpConfigureLogLevel = 11;

// nanopb fixed char[] fields are NUL-terminated by the decoder for STRING
// fields, so a direct std::string(ptr) is safe.
std::string s(const char* z) { return std::string(z); }
}  // namespace

void Supervisor::dispatch_control_nanopb(const void* req_nanopb,
                                         void* rep_nanopb) {
    const auto& req =
        *static_cast<const services_supervisor_ControlRequest*>(req_nanopb);
    auto& rep = *static_cast<services_supervisor_ControlReply*>(rep_nanopb);

    rep.correlation_id = req.correlation_id;
    rep.status = 0;

    auto set_msg = [&rep](const char* m) {
        std::snprintf(rep.message, sizeof(rep.message), "%s", m);
    };
    auto set_child = [&rep](const std::string& n) {
        std::snprintf(rep.child_name, sizeof(rep.child_name), "%s", n.c_str());
    };

    switch (req.op_kind) {
        case kOpStartChild: {
            const auto& spec = req.start_child.spec;
            std::vector<std::string> cmd;
            for (pb_size_t i = 0; i < spec.start_cmd_count; ++i)
                cmd.emplace_back(spec.start_cmd[i]);
            std::vector<std::string> mods;
            for (pb_size_t i = 0; i < spec.modules_count; ++i)
                mods.emplace_back(spec.modules[i]);
            uint32_t status = 0;
            do_start_child(s(spec.parent_supervisor), s(spec.name), cmd,
                           static_cast<int>(spec.restart),
                           static_cast<int>(spec.shutdown),
                           static_cast<int>(spec.type), mods, status);
            rep.status = status;
            set_child(s(spec.name));
            break;
        }
        case kOpDeleteChild:
            rep.status = do_delete_child(s(req.delete_child.name));
            set_child(s(req.delete_child.name));
            break;
        case kOpRestartChild:
            rep.status = do_restart_child(s(req.restart_child.name));
            set_child(s(req.restart_child.name));
            break;
        case kOpTerminateChild:
            rep.status = do_terminate_child(s(req.terminate_child.name));
            set_child(s(req.terminate_child.name));
            break;
        case kOpStop:
            request_shutdown();
            break;
        case kOpConfigureTrace: {
            const auto& cfg = req.configure_trace.config;
            apply_trace_config(s(cfg.target_node), s(cfg.msg_type),
                               cfg.enabled, cfg.kind);
            set_msg("trace config applied");
            break;
        }
        case kOpGetTraceConfig: {
            // Flatten trace_configs_ into ControlReply.trace_config_list (a
            // nanopb bytes field holding a serialized TraceConfigList) — the
            // inline read-back shape com expects.
            services_supervisor_TraceConfigList list =
                services_supervisor_TraceConfigList_init_zero;
            for (const auto& kv : trace_configs_) {
                for (const auto& inner : kv.second) {
                    if (list.configs_count >=
                        (pb_size_t)(sizeof(list.configs) / sizeof(list.configs[0])))
                        break;
                    auto& c = list.configs[list.configs_count++];
                    std::snprintf(c.target_node, sizeof(c.target_node), "%s",
                                  kv.first.c_str());
                    std::snprintf(c.msg_type, sizeof(c.msg_type), "%s",
                                  inner.first.c_str());
                    c.enabled = true;          // presence = enabled
                    c.kind = inner.second;     // value = TraceKind (#403)
                }
            }
            pb_ostream_t os = pb_ostream_from_buffer(
                rep.trace_config_list.bytes, sizeof(rep.trace_config_list.bytes));
            if (pb_encode(&os, services_supervisor_TraceConfigList_fields, &list)) {
                rep.trace_config_list.size = (pb_size_t)os.bytes_written;
            } else {
                rep.status = 5;  // encode overflow — too many entries to fit
                set_msg("trace_config_list encode overflow");
            }
            break;
        }
        case kOpConfigureLogLevel: {
            const auto& cfg = req.configure_log_level.config;
            apply_log_level(s(cfg.target_node), s(cfg.level));
            set_msg("log level applied");
            break;
        }
        default:
            rep.status = 4;  // invalid_request
            set_msg("unknown or two-frame op_kind on the standard transport");
            break;
    }
}

// ---- the gen_server handler ----------------------------------------------

services_supervisor_ControlReply SupervisorControlNode::handle_call(
        const services_supervisor_ControlRequest& req,
        SupervisorControlState& st) {
    services_supervisor_ControlReply rep =
        services_supervisor_ControlReply_init_zero;
    if (!st.sup) {
        rep.status = 14;  // unavailable — no orchestrator bound
        return rep;
    }

    // #431 — we are on the TipcMux epoll thread. dispatch_control_nanopb
    // mutates the supervision tree (do_*/apply_*) and may fork; that must run
    // SINGLE-THREADED on the select() loop, the sole owner of all supervision
    // state. So: build a closure that runs the dispatch + fulfils a promise,
    // post it to the loop's command queue (which wakes the loop), and block
    // here on the future. The req is a fixed nanopb struct — copy it by value
    // into the closure so it outlives this frame. Bounded wait: a wedged loop
    // ⇒ status=14 (unavailable) rather than a hung gRPC caller.
    Supervisor* sup = st.sup;
    auto prom = std::make_shared<std::promise<services_supervisor_ControlReply>>();
    auto fut  = prom->get_future();
    services_supervisor_ControlRequest req_copy = req;  // value capture

    sup->post_command([sup, req_copy, prom]() mutable {
        services_supervisor_ControlReply r =
            services_supervisor_ControlReply_init_zero;
        sup->dispatch_control_nanopb(&req_copy, &r);
        prom->set_value(r);
    });

    if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        return fut.get();
    }
    // Timed out — the loop didn't drain the command in time. Return a
    // unavailable reply rather than block the epoll thread indefinitely.
    rep.status = 14;
    rep.correlation_id = req.correlation_id;
    std::snprintf(rep.message, sizeof(rep.message),
                  "supervisor command queue timeout");
    return rep;
}

// ---- ControlServer (pimpl) -----------------------------------------------
//
// Owns the TipcMux + the SupervisorControlNode and binds them at the control
// address. Confined to this TU so the nanopb ControlRequest.pb.h never meets
// runtime.cpp's libprotobuf one. runtime.cpp drives lifetime via
// control_server.h's opaque start/stop.

struct ControlServer::Impl {
    explicit Impl(Supervisor* sup) : node(sup) {}

    demo::runtime::TipcMux mux;
    SupervisorControlNode  node;
    bool                   started = false;
};

ControlServer::ControlServer(Supervisor* sup)
    : impl_(new Impl(sup)) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
    if (!impl_ || impl_->started) return impl_ && impl_->started;
    impl_->node.start();
    auto* binding = impl_->mux.bind_node(
        impl_->node, SupervisorControlNode::kTipcType,
        SupervisorControlNode::kTipcInstance);
    if (!binding) {
        // TIPC bind failed; tear the node back down and report inert.
        impl_->node.stop("bind-failed");
        return false;
    }
    impl_->mux.register_call<services_supervisor_ControlRequest,
                             services_supervisor_ControlReply>(
        binding, impl_->node);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void ControlServer::stop() {
    if (!impl_ || !impl_->started) return;
    impl_->mux.stop();
    impl_->node.stop("normal");
    impl_->started = false;
}

}  // namespace supervisor
