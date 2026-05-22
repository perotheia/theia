// Load + default machines list. See header for schema.

#include "sup_gui/machines.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <stdexcept>

namespace sup_gui {

std::vector<MachineEndpoint> load_machines_yaml(const std::string& path) {
    std::vector<MachineEndpoint> out;
    YAML::Node doc;
    try {
        doc = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "machines: failed to read %s: %s\n",
                     path.c_str(), e.what());
        return out;
    }
    const YAML::Node& rows = doc["machines"];
    if (!rows || !rows.IsSequence()) {
        std::fprintf(stderr,
                     "machines: %s has no 'machines:' sequence\n",
                     path.c_str());
        return out;
    }
    for (const auto& r : rows) {
        MachineEndpoint ep;
        ep.name    = r["name"]    ? r["name"].as<std::string>()    : "";
        ep.address = r["address"] ? r["address"].as<std::string>() : "127.0.0.1";
        ep.port    = r["port"]    ? static_cast<uint16_t>(r["port"].as<int>())
                                  : 7610;
        if (ep.name.empty()) {
            std::fprintf(stderr,
                         "machines: skipping row with empty 'name'\n");
            continue;
        }
        out.push_back(std::move(ep));
    }
    return out;
}

std::vector<MachineEndpoint> default_machines() {
    return {{ "localhost", "127.0.0.1", 7610 }};
}

}  // namespace sup_gui
