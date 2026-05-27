// Per-machine endpoint list loaded from the JSON emitted by
// `artheia gui emit <rig>`:
//
//   {
//     "machines": [
//       { "name": "demo_host", "address": "127.0.0.1", "port": 7700 }
//     ]
//   }
//
// One GrpcClient is spun per row; the GUI multiplexes their streams.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sup_gui {

struct MachineEndpoint {
    std::string name;
    std::string address;
    uint16_t    port = 7700;
};

std::vector<MachineEndpoint> load_machines_json(const std::string& path);

// Load endpoints from the per-machine layout emitted by
// `artheia generate-manifest`: walks ``<dir>/index.json`` for the
// machine list and reads each ``<dir>/<machine>/machine.json`` for
// the com_endpoint. Skips machines with kind="host" (admin
// workstations have no supervisor to connect to). Returns empty on
// I/O or shape errors (prints a diagnostic to stderr).
std::vector<MachineEndpoint> load_manifest_dir(const std::string& dir);

// Try a sequence of well-known locations for either a flat
// machines.json or a per-machine dist/manifest/ layout. Returns the
// first non-empty hit. Logs each attempt to stderr so the operator
// can see why a particular location was skipped.
std::vector<MachineEndpoint> autodiscover_machines();

std::vector<MachineEndpoint> default_machines();   // localhost:7700

}  // namespace sup_gui
