// Per-machine endpoint list loaded from the YAML emitted by
// `artheia gui emit <rig>`:
//
//   machines:
//   - name: demo_host
//     address: 127.0.0.1
//     port: 7700               # the services/com gRPC port
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

std::vector<MachineEndpoint> load_machines_yaml(const std::string& path);
std::vector<MachineEndpoint> default_machines();   // localhost:7700

}  // namespace sup_gui
