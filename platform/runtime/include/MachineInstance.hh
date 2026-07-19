// MachineInstance — per-NODE TIPC address resolution so the BINARY is the same on
// every machine. A service has a STABLE TIPC type and a per-MACHINE instance
// (central=0, compute=1), so the same FC binary on two machines is two distinct
// cluster addresses (supervisor:0 vs supervisor:1, shwa:0 vs shwa:1).
//
// SOURCE: a command-line ARG the supervisor appends to each child's start_cmd —
//   --tipc=<node>=<type>:<inst>|<node2>=:<inst>|...
// where each entry is FULL "type:inst" (override both) or instance-only ":inst"
// (keep the compiled type, override the instance). ARG-ONLY: no env var. main()
// calls set_node_tipc_arg(argc, argv) once; each node then resolves via
// resolve_node_tipc(), falling back to its compiled kTipcType/kTipcInstance when
// the ARG is absent (a bare / un-supervised run binds exactly the .art address —
// host-dev unchanged).

#pragma once

#include <cstdint>

namespace theia {
namespace runtime {

// Parse the --tipc=... argv entry (if present) into a process-global node→address
// map. Call once at the top of main(), before any node binds. Idempotent. A
// missing --tipc leaves the map empty (every node falls back to its compiled
// address). Accepts both --tipc=VALUE and "--tipc VALUE" forms.
void set_node_tipc_arg(int argc, char** argv) noexcept;

// Resolve a node's TIPC address. Looks node_name up in the parsed --tipc map; an
// entry may carry a full type:inst or an instance-only :inst (then default_type
// is kept). Falls back to the compiled (default_type, default_instance) when the
// node isn't in the arg. out_type/out_instance receive the result. Returns true
// if resolved from the ARG, false if it fell back to the compiled defaults.
bool resolve_node_tipc(const char* node_name,
                       uint32_t default_type, uint32_t default_instance,
                       uint32_t& out_type, uint32_t& out_instance) noexcept;

// This machine's cluster index, read from $THEIA_MACHINE_INSTANCE (central=0,
// compute=1, …); 0 when unset/empty/malformed. The supervisor shifts each
// CHILD's --tipc instance by this (runtime.cpp), so a child's address is already
// machine-correct. But the TOP-LEVEL supervisor takes NO --tipc for its OWN
// ctl/worker nodes — theia-run.sh just execs it — so resolve_node_tipc falls
// back to the compiled instance 0 on EVERY board, colliding two supervisors on
// 0x80020001:0 in a shared TIPC namespace. main() applies this offset to the
// supervisor's own ctl/worker ONLY on that fallback (no --tipc override), so the
// master binds :0 and each zonal binds :machine — realizing the design com's
// sup_link.hpp already assumes (LOCAL=0-relative, for_instance(N) reaches board
// N; PG members targeting :0 deterministically reach the master).
unsigned machine_instance_offset() noexcept;

}  // namespace runtime
}  // namespace theia
