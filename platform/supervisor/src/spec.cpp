// JSON → Node tree loader. Mirrors _load_node() in supervisor/runtime.py.
//
// Source-of-truth manifests are emitted by `artheia executor emit` /
// `artheia generate-manifest` as JSON (single canonical format —
// YAML was dropped from the system in #380).

#include "supervisor/spec.h"

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

ChildType parse_child_type(const std::string& s) {
    if (s == "worker")     return ChildType::Worker;
    if (s == "supervisor") return ChildType::Supervisor;
    throw std::runtime_error("unknown child type: " + s);
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

const char* to_string(RestartType t) {
    switch (t) {
        case RestartType::Permanent: return "permanent";
        case RestartType::Transient: return "transient";
        case RestartType::Temporary: return "temporary";
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

std::unique_ptr<Node> load_manifest(const std::string& path) {
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
    return load_node(root, nullptr);
}

}  // namespace supervisor
