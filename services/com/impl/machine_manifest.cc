// machine_manifest implementation — parse THEIA_MACHINE_MANIFEST into an
// instance→name map. See machine_manifest.hpp for the contract.

#include "impl/machine_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace services_com {

namespace {

using nlohmann::json;

// Load <dir>/machine.json and return its Machine.machine_index, or -1 if the
// file is missing / unparseable / has no index.
long read_machine_index(const std::string& dir) {
    std::ifstream f(dir + "/machine.json");
    if (!f) return -1;
    try {
        json j; f >> j;
        // The MachineManifest wraps the Machine under "machine".
        const auto& m = j.contains("machine") ? j.at("machine") : j;
        if (m.contains("machine_index") && m.at("machine_index").is_number())
            return m.at("machine_index").get<long>();
    } catch (...) { /* malformed — treat as absent */ }
    return -1;
}

}  // namespace

struct MachineManifest::Impl {
    std::unordered_map<uint32_t, std::string> by_instance;
    bool                                      loaded = false;

    void load() {
        const char* root = std::getenv("THEIA_MACHINE_MANIFEST");
        if (!root || !*root) return;   // env unset → fallback names

        std::ifstream f(std::string(root) + "/machines.json");
        if (!f) return;
        try {
            json j; f >> j;
            if (!j.contains("machines") || !j.at("machines").is_array()) return;
            for (const auto& m : j.at("machines")) {
                const std::string name =
                    m.value("name", std::string());
                const std::string subdir =
                    m.value("manifests_dir", name);   // dir defaults to name
                if (name.empty() || subdir.empty()) continue;
                long idx = read_machine_index(std::string(root) + "/" + subdir);
                if (idx < 0) continue;                // host/admin w/o index — skip
                by_instance[static_cast<uint32_t>(idx)] = name;
                loaded = true;
            }
        } catch (...) { /* malformed machines.json — fall back to mN */ }
    }
};

MachineManifest::MachineManifest() : impl_(new Impl()) {
    impl_->load();
    if (impl_->loaded) {
        std::string names;
        for (const auto& kv : impl_->by_instance)
            names += " " + std::to_string(kv.first) + "=" + kv.second;
        std::fprintf(stderr,
            "[com] machine manifest loaded (%zu machines:%s)\n",
            impl_->by_instance.size(), names.c_str());
    } else {
        std::fprintf(stderr,
            "[com] no machine manifest (THEIA_MACHINE_MANIFEST unset/empty) — "
            "machines labelled mN\n");
    }
}

MachineManifest& MachineManifest::instance() {
    static MachineManifest s;
    return s;
}

std::string MachineManifest::name(uint32_t inst) const {
    auto it = impl_->by_instance.find(inst);
    if (it != impl_->by_instance.end()) return it->second;
    return "m" + std::to_string(inst);   // synthetic fallback (Stage-3 prefix)
}

bool MachineManifest::loaded() const { return impl_->loaded; }

}  // namespace services_com
