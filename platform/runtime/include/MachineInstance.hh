// MachineInstance — the cluster machine index (central=0, compute=1, …) every
// node on this machine binds as its TIPC INSTANCE. A service has a STABLE TIPC
// type and a per-machine instance, so the same FC binary on two machines is two
// distinct cluster addresses (supervisor:0 vs supervisor:1, shwa:0 vs shwa:1) —
// "same binary modulo arch", the instance assigned at deploy time, NOT baked
// into a per-machine class.
//
// Source: the THEIA_MACHINE_INSTANCE spawn env. The supervisor sets it on every
// child from the machine manifest's machine_index, and binds its own nodes at it
// too. Absent/unset = 0 (single-machine + legacy rigs unchanged). Read once,
// cached, so every node in the process agrees.

#pragma once

#include <cstdint>

namespace theia {
namespace runtime {

// This process's machine instance (0 if THEIA_MACHINE_INSTANCE is unset/blank).
uint32_t machine_instance() noexcept;

// A node's effective TIPC instance = its declared (compiled) instance + this
// machine's index. The .art declares instance 0; the deploy shifts it per
// machine. Use at every bind/listen site instead of the raw kTipcInstance.
inline uint32_t effective_instance(uint32_t declared_instance) noexcept {
    return declared_instance + machine_instance();
}

// A node's TIPC ADDRESS resolved from the env, so the BINARY is address-agnostic.
// The supervisor sets THEIA_NODE_TIPC = "<node>=<type>:<inst>|<node2>=..." per
// child from executor.json, with the instance already machine-shifted. main.cc
// passes the node's kNodeName + its compiled kTipcType/kTipcInstance as the
// fallback (used for a standalone / un-supervised run where the env is absent).
//
// out_type/out_instance receive the resolved address. Returns true if the node
// was found in THEIA_NODE_TIPC (env-driven), false if it fell back to the
// defaults (with machine_instance() still applied to the default instance, so an
// env-less but THEIA_MACHINE_INSTANCE-set run still shifts correctly).
bool resolve_node_tipc(const char* node_name,
                       uint32_t default_type, uint32_t default_instance,
                       uint32_t& out_type, uint32_t& out_instance) noexcept;

}  // namespace runtime
}  // namespace theia
