// machine_manifest implementation — parse THEIA_MACHINE_MANIFEST into an
// instance→name map. See machine_manifest.hpp for the contract.

#include "impl/machine_manifest.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
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
    std::unordered_map<uint32_t, std::string> by_instance;   // index → name
    std::unordered_map<uint32_t, std::string> role_by_inst;  // index → role
    bool                                      loaded = false;

    void load() {
        const char* root = std::getenv("THEIA_MACHINE_MANIFEST");
        if (!root || !*root) return;   // env unset → fallback names

        std::ifstream f(std::string(root) + "/machines.json");
        if (!f) return;
        try {
            json j; f >> j;
            // PREFERRED: the inline name → machine_index map (serialize-manifest
            // emits it into machines.json — the ONE file com references). This is
            // the authoritative correlation: com discovers a supervisor at TIPC
            // instance N and maps N → the UNIQUE machine NAME (the runtime identity;
            // role and hostname are NOT unique). No per-machine machine.json read.
            if (j.contains("machine_index") && j.at("machine_index").is_object()) {
                for (const auto& kv : j.at("machine_index").items()) {
                    if (!kv.value().is_number()) continue;
                    const auto idx = kv.value().get<long>();
                    if (idx < 0) continue;
                    by_instance[static_cast<uint32_t>(idx)] = kv.key();  // idx → name
                    loaded = true;
                }
            }
            // role_map (name → role, master/zonal) → role_by_inst (index → role),
            // resolving names through the machine_index we just built. The role is
            // the DEPLOYMENT identity — non-unique, informational for the GUI.
            if (loaded && j.contains("role_map") && j.at("role_map").is_object()) {
                for (const auto& kv : j.at("machine_index").items()) {
                    if (!kv.value().is_number()) continue;
                    const auto idx = static_cast<uint32_t>(kv.value().get<long>());
                    const auto rit = j.at("role_map").find(kv.key());
                    if (rit != j.at("role_map").end() && rit->is_string())
                        role_by_inst[idx] = rit->get<std::string>();
                }
            }
            if (loaded) return;   // inline map is complete — done.

            // LEGACY fallback: older machines.json without the inline index —
            // "machines" is a NAME array; recover each name's index from its
            // per-machine machine.json (needs the sibling <name>/machine.json).
            if (!j.contains("machines") || !j.at("machines").is_array()) return;
            for (const auto& m : j.at("machines")) {
                std::string name;
                if (m.is_string())      name = m.get<std::string>();
                else if (m.is_object()) name = m.value("name", std::string());
                if (name.empty()) continue;
                long idx = read_machine_index(std::string(root) + "/" + name);
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

std::string MachineManifest::role(uint32_t inst) const {
    auto it = impl_->role_by_inst.find(inst);
    return it != impl_->role_by_inst.end() ? it->second : std::string();
}

bool MachineManifest::index_of(const std::string& nm, uint32_t& out) const {
    for (const auto& kv : impl_->by_instance)
        if (kv.second == nm) { out = kv.first; return true; }
    // Synthetic "mN" form (the name() fallback) — parse the digits.
    if (nm.size() > 1 && nm[0] == 'm') {
        bool all_digits = true;
        for (size_t i = 1; i < nm.size(); ++i)
            if (!std::isdigit((unsigned char)nm[i])) { all_digits = false; break; }
        if (all_digits) {
            out = (uint32_t)std::strtoul(nm.c_str() + 1, nullptr, 10);
            return true;
        }
    }
    return false;
}

bool MachineManifest::loaded() const { return impl_->loaded; }

bool MachineManifest::has(uint32_t inst) const {
    return impl_->by_instance.find(inst) != impl_->by_instance.end();
}

}  // namespace services_com
