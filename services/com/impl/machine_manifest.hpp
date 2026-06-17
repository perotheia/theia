// machine_manifest — com's read-only view of the CLUSTER machine list.
//
// com on central aggregates every machine's telemetry (Stage 3 supervisor tree
// fan-out + the per-machine AccelTelemetry demux). To label those by a human
// MACHINE NAME rather than a bare TIPC instance, com needs the cluster's
// index→name map. The supervisor exports THEIA_MACHINE_MANIFEST (the manifest
// ROOT dir) and every child — including com — inherits it; this reader parses
//   <root>/machines.json          (the RigIndex: machine name + manifests_dir)
//   <root>/<manifests_dir>/machine.json   (Machine.machine_index)
// into a flat instance→name lookup. Best-effort: if the env is unset or the
// files are missing, name(i) falls back to "mN" (the Stage-3 prefix) so a
// central-only / manifest-less dev stack still works.
//
// nlohmann/json stays inside the .cc; this header is plain types only.

#pragma once

#include <cstdint>
#include <string>

namespace services_com {

// Process-wide singleton, loaded once on first use from THEIA_MACHINE_MANIFEST.
class MachineManifest {
public:
    static MachineManifest& instance();

    // Human name for a machine TIPC instance (central=0, compute=1, …). Returns
    // the manifest name if known, else a synthetic "mN" (N=inst) so callers
    // always get a stable, non-empty label. Thread-safe (immutable after load).
    std::string name(uint32_t instance) const;

    // Was a manifest actually loaded (vs the env-unset fallback)? For logging.
    bool loaded() const;

private:
    MachineManifest();
    MachineManifest(const MachineManifest&)            = delete;
    MachineManifest& operator=(const MachineManifest&) = delete;

    struct Impl;
    Impl* impl_;
};

}  // namespace services_com
