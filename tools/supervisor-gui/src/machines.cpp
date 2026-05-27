#include "sup_gui/machines.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

namespace sup_gui {

namespace fs = std::filesystem;

namespace {

bool load_json_file(const std::string& path, json& out) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "machines: failed to open %s\n", path.c_str());
        return false;
    }
    try {
        f >> out;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "machines: failed to parse %s: %s\n",
                     path.c_str(), e.what());
        return false;
    }
    return true;
}

std::string jstr(const json& j, const char* key, const std::string& dflt = "") {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_string()) return dflt;
    return it->get<std::string>();
}

int jint(const json& j, const char* key, int dflt) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number_integer()) return dflt;
    return it->get<int>();
}

}  // namespace

std::vector<MachineEndpoint> load_machines_json(const std::string& path) {
    std::vector<MachineEndpoint> out;
    json doc;
    if (!load_json_file(path, doc)) return out;
    auto rows = doc.find("machines");
    if (rows == doc.end() || !rows->is_array()) {
        std::fprintf(stderr,
                     "machines: %s has no 'machines' array\n", path.c_str());
        return out;
    }
    for (const auto& r : *rows) {
        MachineEndpoint ep;
        ep.name    = jstr(r, "name");
        ep.address = jstr(r, "address", "127.0.0.1");
        ep.port    = static_cast<uint16_t>(jint(r, "port", 7700));
        if (ep.name.empty()) continue;
        out.push_back(std::move(ep));
    }
    return out;
}

std::vector<MachineEndpoint> load_manifest_dir(const std::string& dir) {
    // Walk the per-machine manifest layout emitted by
    // `artheia generate-manifest --out <dir>`:
    //
    //   <dir>/index.json                     ← list of machines
    //   <dir>/<machine>/machine.json         ← com_endpoint per machine
    //
    // Use index.json to find machines (including their `kind`) and
    // each <machine>/machine.json for the real address+port. Skip
    // kind="host" machines — the GUI runs on those; nothing to
    // connect to.
    std::vector<MachineEndpoint> out;
    const fs::path root(dir);
    const fs::path index = root / "index.json";
    if (!fs::is_regular_file(index)) {
        std::fprintf(stderr, "machines: no index.json under %s\n", dir.c_str());
        return out;
    }
    json idx;
    if (!load_json_file(index.string(), idx)) return out;
    auto rows = idx.find("machines");
    if (rows == idx.end() || !rows->is_array()) {
        std::fprintf(stderr, "machines: %s has no 'machines' array\n",
                     index.c_str());
        return out;
    }
    for (const auto& r : *rows) {
        const std::string name = jstr(r, "name");
        const std::string kind = jstr(r, "kind");
        const std::string sub  = jstr(r, "manifests_dir", name);
        if (name.empty()) continue;
        if (kind == "host") {
            std::fprintf(stderr, "machines: skipping host machine '%s'\n",
                         name.c_str());
            continue;
        }
        const fs::path mfile = root / sub / "machine.json";
        if (!fs::is_regular_file(mfile)) {
            std::fprintf(stderr, "machines: %s missing — skipping %s\n",
                         mfile.c_str(), name.c_str());
            continue;
        }
        json mdoc;
        if (!load_json_file(mfile.string(), mdoc)) continue;
        auto machine = mdoc.find("machine");
        if (machine == mdoc.end() || !machine->is_object()) {
            std::fprintf(stderr, "machines: %s has no 'machine' object\n",
                         mfile.c_str());
            continue;
        }
        auto ce = machine->find("com_endpoint");
        MachineEndpoint ep;
        ep.name = name;
        if (ce != machine->end() && ce->is_object()) {
            ep.address = jstr(*ce, "address", "127.0.0.1");
            ep.port    = static_cast<uint16_t>(jint(*ce, "port", 7700));
        } else {
            ep.address = "127.0.0.1";
            ep.port    = 7700;
        }
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
        "machines.json",                       // cwd
        "/etc/theia/machines.json",            // installed admin .deb location
    };
    for (const auto& f : flat_files) {
        if (!fs::is_regular_file(f)) continue;
        std::fprintf(stderr, "machines: trying %s (flat)\n", f.c_str());
        auto m = load_machines_json(f);
        if (!m.empty()) return m;
    }
    return {};
}

std::vector<MachineEndpoint> default_machines() {
    return {{ "localhost", "127.0.0.1", 7700 }};
}

}  // namespace sup_gui
