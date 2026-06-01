#include "bridge.h"

#include <atomic>

namespace supervisor {

namespace {
std::atomic<Supervisor*> g_supervisor{nullptr};
EmitForwarder            g_forwarder{};  // function-pointer slots, value-init
}  // namespace

void set_supervisor(Supervisor* sup) { g_supervisor.store(sup); }

Supervisor* supervisor_instance() { return g_supervisor.load(); }

void set_emit_forwarder(const EmitForwarder& fwd) { g_forwarder = fwd; }

const EmitForwarder& emit_forwarder() { return g_forwarder; }

}  // namespace supervisor
