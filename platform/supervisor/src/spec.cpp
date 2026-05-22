// YAML → Node tree loader. Mirrors _load_node() in supervisor/runtime.py.

#include "supervisor/spec.h"

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <string>

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

Shutdown read_shutdown(const YAML::Node& y) {
    if (!y) return Shutdown::timeout(5000);
    if (y.IsScalar()) {
        const std::string s = y.Scalar();
        if (s == "brutal_kill") return Shutdown::brutal_kill();
        if (s == "infinity")    return Shutdown::infinity();
        // numeric literal (could parse as int).
        try {
            return Shutdown::timeout(std::stoi(s));
        } catch (const std::exception&) {
            throw std::runtime_error("unparseable shutdown value: " + s);
        }
    }
    throw std::runtime_error("shutdown must be scalar");
}

std::unique_ptr<Node> load_node(const YAML::Node& y, SupervisorNode* parent);

std::unique_ptr<Node> load_supervisor(const YAML::Node& y, SupervisorNode* parent) {
    SupervisorNode s;
    s.name         = y["name"].as<std::string>();
    s.strategy     = parse_strategy(y["strategy"].as<std::string>("one_for_one"));
    s.max_restarts = y["max_restarts"].as<int>(3);
    s.max_seconds  = y["max_seconds"].as<int>(5);
    s.parent       = parent;
    if (y["tombstone_dir"]) s.tombstone_dir = y["tombstone_dir"].as<std::string>();
    // listen_port: ignored — TCP transport has been removed; the
    // services/com bridge in phase 2 will host the GUI-facing endpoint
    // and read its TCP/gRPC port from its own machine manifest entry.

    auto node = Node::make_supervisor(std::move(s));
    SupervisorNode* self = &node->sup;

    for (const auto& c : y["children"]) {
        self->children.push_back(load_node(c, self));
    }
    return node;
}

std::unique_ptr<Node> load_worker(const YAML::Node& y) {
    WorkerNode w;
    w.name = y["name"].as<std::string>();
    for (const auto& a : y["start_cmd"]) {
        w.start_cmd.push_back(a.as<std::string>());
    }
    w.restart  = parse_restart_type(y["restart"].as<std::string>("permanent"));
    w.shutdown = read_shutdown(y["shutdown"]);
    if (y["modules"]) {
        for (const auto& m : y["modules"]) w.modules.push_back(m.as<std::string>());
    }
    if (y["env"]) {
        for (auto it = y["env"].begin(); it != y["env"].end(); ++it) {
            w.env[it->first.as<std::string>()] = it->second.as<std::string>();
        }
    }
    if (y["working_dir"]) w.working_dir = y["working_dir"].as<std::string>();
    if (y["shall_run_on"]) {
        for (const auto& c : y["shall_run_on"]) w.shall_run_on.push_back(c.as<int>());
    }
    if (y["shall_not_run_on"]) {
        for (const auto& c : y["shall_not_run_on"]) w.shall_not_run_on.push_back(c.as<int>());
    }
    if (!w.shall_run_on.empty() && !w.shall_not_run_on.empty()) {
        throw std::runtime_error(
            "child '" + w.name + "': shall_run_on and shall_not_run_on are mutually exclusive");
    }
    return Node::make_worker(std::move(w));
}

std::unique_ptr<Node> load_node(const YAML::Node& y, SupervisorNode* parent) {
    if (y["children"]) return load_supervisor(y, parent);
    return load_worker(y);
}

}  // namespace

std::unique_ptr<Node> load_manifest(const std::string& path) {
    YAML::Node root = YAML::LoadFile(path);
    if (!root["children"]) {
        throw std::runtime_error("manifest root must be a supervisor (have 'children')");
    }
    return load_node(root, nullptr);
}

}  // namespace supervisor
