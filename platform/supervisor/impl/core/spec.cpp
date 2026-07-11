// JSON → Node tree loader. Mirrors _load_node() in supervisor/runtime.py.
//
// Source-of-truth manifests are emitted by `artheia executor emit` /
// `artheia generate-manifest` as JSON (single canonical format —
// YAML was dropped from the system in #380).

#include "spec.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

using nlohmann::json;

namespace supervisor {

RestartStrategy parse_strategy(const std::string& s) {
    if (s == "one_for_one")        return RestartStrategy::OneForOne;
    if (s == "one_for_all")        return RestartStrategy::OneForAll;
    if (s == "rest_for_one")       return RestartStrategy::RestForOne;
    if (s == "simple_one_for_one") return RestartStrategy::SimpleOneForOne;
    throw std::runtime_error("unknown restart strategy: " + s);
}

RestartType parse_restart_type(const std::string& s) {
    if (s == "permanent") return RestartType::Permanent;
    if (s == "transient") return RestartType::Transient;
    if (s == "temporary") return RestartType::Temporary;
    throw std::runtime_error("unknown restart type: " + s);
}

const char* to_string(RestartStrategy s) {
    switch (s) {
        case RestartStrategy::OneForOne:        return "one_for_one";
        case RestartStrategy::OneForAll:        return "one_for_all";
        case RestartStrategy::RestForOne:       return "rest_for_one";
        case RestartStrategy::SimpleOneForOne:  return "simple_one_for_one";
    }
    return "<unknown>";
}

namespace {

// JSON helpers — keep call sites readable without sprinkling .value()
// noise everywhere. Default-on-missing matches the YAML loader's
// y["x"].as<T>(default) idiom.

std::string get_str(const json& j, const char* key, const std::string& dflt = "") {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    return it->get<std::string>();
}

int get_int(const json& j, const char* key, int dflt) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    return it->get<int>();
}

bool get_bool(const json& j, const char* key, bool dflt) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return dflt;
    return it->get<bool>();
}

Shutdown read_shutdown(const json& j) {
    if (j.is_null()) return Shutdown::timeout(5000);
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "brutal_kill") return Shutdown::brutal_kill();
        if (s == "infinity")    return Shutdown::infinity();
        try {
            return Shutdown::timeout(std::stoi(s));
        } catch (const std::exception&) {
            throw std::runtime_error("unparseable shutdown value: " + s);
        }
    }
    if (j.is_number_integer()) return Shutdown::timeout(j.get<int>());
    throw std::runtime_error("shutdown must be int or string");
}

std::unique_ptr<Node> load_node(const json& j, SupervisorNode* parent);

std::unique_ptr<Node> load_supervisor(const json& j, SupervisorNode* parent) {
    SupervisorNode s;
    s.name         = get_str(j, "name");
    s.strategy     = parse_strategy(get_str(j, "strategy", "one_for_one"));
    s.max_restarts = get_int(j, "max_restarts", 3);
    s.max_seconds  = get_int(j, "max_seconds", 5);
    s.parent       = parent;
    s.tombstone_dir = get_str(j, "tombstone_dir");

    auto node = Node::make_supervisor(std::move(s));
    SupervisorNode* self = &node->sup;

    auto it = j.find("children");
    if (it != j.end() && it->is_array()) {
        for (const auto& c : *it) {
            self->children.push_back(load_node(c, self));
        }
    }
    return node;
}

std::unique_ptr<Node> load_worker(const json& j) {
    WorkerNode w;
    w.name = get_str(j, "name");

    auto sc = j.find("start_cmd");
    if (sc != j.end() && sc->is_array()) {
        for (const auto& a : *sc) w.start_cmd.push_back(a.get<std::string>());
    }
    w.restart  = parse_restart_type(get_str(j, "restart", "permanent"));
    w.shutdown = read_shutdown(j.value("shutdown", json()));
    // run_on_start (default true): false = define-but-don't-boot (see spec.h).
    w.run_on_start = get_bool(j, "run_on_start", true);

    // Per-process memory cap (RLIMIT_AS), bytes. 0/absent = no cap.
    w.mem_limit_bytes = j.value("mem_limit_bytes", static_cast<uint64_t>(0));

    auto modules = j.find("modules");
    if (modules != j.end() && modules->is_array()) {
        for (const auto& m : *modules) w.modules.push_back(m.get<std::string>());
    }

    auto env = j.find("env");
    if (env != j.end() && env->is_object()) {
        for (auto it = env->begin(); it != env->end(); ++it) {
            w.env[it.key()] = it.value().get<std::string>();
        }
    }

    // Per-process logger kind (executor.json `logger: "file:/path"|"syslog"|
    // "null"|"stdio"`). Flows to the child as the THEIA_LOGGER env var, which
    // the child's main.cc passes to theia::runtime::MakeLogger() at boot. An
    // explicit env["THEIA_LOGGER"] (above) wins if both are present.
    if (std::string lg = get_str(j, "logger"); !lg.empty()) {
        w.env.emplace("THEIA_LOGGER", lg);  // emplace = don't clobber an env entry
    }

    w.working_dir = get_str(j, "working_dir");

    auto sro = j.find("shall_run_on");
    if (sro != j.end() && sro->is_array()) {
        for (const auto& c : *sro) w.shall_run_on.push_back(c.get<int>());
    }
    auto snro = j.find("shall_not_run_on");
    if (snro != j.end() && snro->is_array()) {
        for (const auto& c : *snro) w.shall_not_run_on.push_back(c.get<int>());
    }
    if (!w.shall_run_on.empty() && !w.shall_not_run_on.empty()) {
        throw std::runtime_error(
            "child '" + w.name + "': shall_run_on and shall_not_run_on are mutually exclusive");
    }

    // Per-art-node metadata (#366) — one entry per `node atomic ...`
    // in the FC's .art. Empty list = no reporting (placeholder FC);
    // the synthesis path in emit_snapshot() (#364) skips workers
    // with no reporting nodes.
    auto nodes = j.find("nodes");
    if (nodes != j.end() && nodes->is_array()) {
        for (const auto& n : *nodes) {
            NodeInfo ni;
            ni.name = get_str(n, "name");
            ni.reporting = get_bool(n, "reporting", true);
            ni.tipc_type = get_str(n, "tipc_type");
            ni.tipc_instance = get_str(n, "tipc_instance");
            // Per-node affinity + scheduler (optional). cpus: [int,...];
            // sched: "fifo"|"rr"|...; sched_prio: rtprio.
            if (auto cpus = n.find("cpus");
                cpus != n.end() && cpus->is_array()) {
                for (const auto& c : *cpus) {
                    if (c.is_number_integer()) ni.cpus.push_back(c.get<int>());
                }
            }
            ni.sched      = get_str(n, "sched");
            ni.sched_prio = get_int(n, "sched_prio", 0);
            w.nodes.push_back(std::move(ni));
        }
    }
    return Node::make_worker(std::move(w));
}

std::unique_ptr<Node> load_node(const json& j, SupervisorNode* parent) {
    if (j.contains("children")) return load_supervisor(j, parent);
    return load_worker(j);
}

}  // namespace

std::unique_ptr<Node> load_manifest(const std::string& path,
                                    std::string* machine_out) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    json root;
    try {
        f >> root;
    } catch (const json::exception& e) {
        throw std::runtime_error("JSON parse failed for " + path + ": " + e.what());
    }
    if (!root.contains("children")) {
        throw std::runtime_error("manifest root must be a supervisor (have 'children')");
    }
    // The machine this executor.json is for (serialize-manifest stamps it on the
    // root). The supervisor reports it in GetSystemInfo so com labels per-machine
    // telemetry by the REAL name — no separate manifest/env lookup. Optional:
    // a hand-written / legacy executor.json without it just yields "".
    if (machine_out)
        *machine_out = root.value("machine", std::string());
    return load_node(root, nullptr);
}

}  // namespace supervisor
