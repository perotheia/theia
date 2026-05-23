#include "sup_gui/machines.h"

#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace sup_gui {

namespace fs = std::filesystem;

std::vector<MachineEndpoint> load_machines_yaml(const std::string& path) {
    std::vector<MachineEndpoint> out;
    YAML::Node doc;
    try {
        doc = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "machines: failed to read %s: %s\n",
                     path.c_str(), e.what());
        return out;
    }
    const YAML::Node& rows = doc["machines"];
    if (!rows || !rows.IsSequence()) {
        std::fprintf(stderr,
                     "machines: %s has no 'machines:' sequence\n", path.c_str());
        return out;
    }
    for (const auto& r : rows) {
        MachineEndpoint ep;
        ep.name    = r["name"]    ? r["name"].as<std::string>()    : "";
        ep.address = r["address"] ? r["address"].as<std::string>() : "127.0.0.1";
        ep.port    = r["port"]    ? static_cast<uint16_t>(r["port"].as<int>())
                                  : 7700;
        if (ep.name.empty()) continue;
        out.push_back(std::move(ep));
    }
    return out;
}

std::vector<MachineEndpoint> load_manifest_dir(const std::string& dir) {
    // Walk the per-machine manifest layout emitted by
    // `artheia generate-manifest --out <dir>`:
    //
    //   <dir>/index.yaml                     ← list of machines
    //   <dir>/<machine>/machine.yaml         ← com_endpoint per machine
    //
    // We use index.yaml to find machines (including their `kind`) and
    // each <machine>/machine.yaml for the real address+port. Skip
    // kind="host" machines — the GUI runs on those; nothing to connect
    // to.
    std::vector<MachineEndpoint> out;
    const fs::path root(dir);
    const fs::path index = root / "index.yaml";
    if (!fs::is_regular_file(index)) {
        std::fprintf(stderr, "machines: no index.yaml under %s\n", dir.c_str());
        return out;
    }
    YAML::Node idx;
    try {
        idx = YAML::LoadFile(index.string());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "machines: failed to read %s: %s\n",
                     index.c_str(), e.what());
        return out;
    }
    const YAML::Node& rows = idx["machines"];
    if (!rows || !rows.IsSequence()) {
        std::fprintf(stderr, "machines: %s has no 'machines:' sequence\n",
                     index.c_str());
        return out;
    }
    for (const auto& r : rows) {
        const std::string name = r["name"] ? r["name"].as<std::string>() : "";
        const std::string kind = r["kind"] ? r["kind"].as<std::string>() : "";
        const std::string sub = r["manifests_dir"]
            ? r["manifests_dir"].as<std::string>()
            : name;
        if (name.empty()) continue;
        if (kind == "host") {
            std::fprintf(stderr, "machines: skipping host machine '%s'\n",
                         name.c_str());
            continue;
        }
        const fs::path mfile = root / sub / "machine.yaml";
        if (!fs::is_regular_file(mfile)) {
            std::fprintf(stderr, "machines: %s missing — skipping %s\n",
                         mfile.c_str(), name.c_str());
            continue;
        }
        YAML::Node mdoc;
        try {
            mdoc = YAML::LoadFile(mfile.string());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "machines: failed to read %s: %s\n",
                         mfile.c_str(), e.what());
            continue;
        }
        const YAML::Node m = mdoc["machine"];
        if (!m) {
            std::fprintf(stderr, "machines: %s has no 'machine:' key\n",
                         mfile.c_str());
            continue;
        }
        const YAML::Node ce = m["com_endpoint"];
        MachineEndpoint ep;
        ep.name    = name;
        ep.address = (ce && ce["address"])
                     ? ce["address"].as<std::string>() : "127.0.0.1";
        ep.port    = (ce && ce["port"])
                     ? static_cast<uint16_t>(ce["port"].as<int>()) : 7700;
        out.push_back(std::move(ep));
    }
    return out;
}

std::vector<MachineEndpoint> autodiscover_machines() {
    // Search order (first non-empty wins). Each step logs to stderr
    // so the operator can see which path was taken.
    const std::vector<std::string> manifest_dirs = {
        "dist/manifest",                       // dev workspace cwd
        "/etc/theia/manifest",                 // installed .deb location
    };
    for (const auto& d : manifest_dirs) {
        if (!fs::is_directory(d)) continue;
        std::fprintf(stderr, "machines: trying %s/ (per-machine layout)\n",
                     d.c_str());
        auto m = load_manifest_dir(d);
        if (!m.empty()) return m;
    }
    const std::vector<std::string> flat_files = {
        "machines.yaml",                       // cwd
        "/etc/theia/machines.yaml",            // legacy installed path
    };
    for (const auto& f : flat_files) {
        if (!fs::is_regular_file(f)) continue;
        std::fprintf(stderr, "machines: trying %s (flat)\n", f.c_str());
        auto m = load_machines_yaml(f);
        if (!m.empty()) return m;
    }
    return {};
}

std::vector<MachineEndpoint> default_machines() {
    return {{ "localhost", "127.0.0.1", 7700 }};
}

}  // namespace sup_gui
