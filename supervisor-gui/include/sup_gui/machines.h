// Machine endpoint list — loaded from machines.yaml emitted by
// `artheia gui emit`.
//
// Schema (per row):
//
//   machines:
//   - name: demo_host
//     address: 127.0.0.1
//     port: 7610
//
// The GUI instantiates one TcpClient per row.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sup_gui {

struct MachineEndpoint {
    std::string name;     // human-readable machine identity
    std::string address;  // dotted-quad or DNS name
    uint16_t    port = 7610;
};

// Loaded list. Returns an empty vector on parse failure or empty file —
// the GUI runs anyway, just without any connections.
std::vector<MachineEndpoint> load_machines_yaml(const std::string& path);

// Fallback used when no manifest is given: a single localhost row on
// the default port. Lets the GUI run as before without configuration.
std::vector<MachineEndpoint> default_machines();

}  // namespace sup_gui
