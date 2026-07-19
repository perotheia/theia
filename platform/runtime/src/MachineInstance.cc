// MachineInstance — see MachineInstance.hh. ARG-only per-node TIPC resolution.

#include "MachineInstance.hh"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace theia {
namespace runtime {

namespace {

struct Addr {
    bool     has_type;     // false → instance-only entry (keep compiled type)
    uint32_t type;
    uint32_t instance;
};

std::map<std::string, Addr>& tipc_map() {
    static std::map<std::string, Addr> m;
    return m;
}
std::once_flag g_parsed;

// Parse one "<type>:<inst>" or ":<inst>" value into an Addr.
bool parse_addr(const char* val, Addr& out) {
    const char* colon = std::strchr(val, ':');
    if (!colon) return false;
    if (colon == val) {
        // ":<inst>" — instance only.
        out.has_type = false;
        out.type     = 0;
    } else {
        out.has_type = true;
        out.type     = static_cast<uint32_t>(std::strtoul(val, nullptr, 0));  // 0x… ok
    }
    out.instance = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 10));
    return true;
}

// Parse the whole "--tipc" value: "n1=type:inst|n2=:inst|...".
void parse_tipc_value(const char* value) {
    auto& m = tipc_map();
    const char* p = value;
    while (p && *p) {
        const char* bar = std::strchr(p, '|');
        const char* end = bar ? bar : p + std::strlen(p);
        const char* eq  = std::strchr(p, '=');
        if (eq && eq < end) {
            std::string node(p, static_cast<size_t>(eq - p));
            std::string val(eq + 1, static_cast<size_t>(end - (eq + 1)));
            Addr a{};
            if (parse_addr(val.c_str(), a)) m[node] = a;
        }
        if (!bar) break;
        p = bar + 1;
    }
}

}  // namespace

void set_node_tipc_arg(int argc, char** argv) noexcept {
    std::call_once(g_parsed, [argc, argv] {
        for (int i = 1; i < argc; ++i) {
            const char* a = argv[i];
            if (std::strncmp(a, "--tipc=", 7) == 0) {
                parse_tipc_value(a + 7);
            } else if (std::strcmp(a, "--tipc") == 0 && i + 1 < argc) {
                parse_tipc_value(argv[++i]);
            }
        }
    });
}

bool resolve_node_tipc(const char* node_name,
                       uint32_t default_type, uint32_t default_instance,
                       uint32_t& out_type, uint32_t& out_instance) noexcept {
    out_type     = default_type;
    out_instance = default_instance;
    if (!node_name) return false;
    auto& m = tipc_map();
    auto it = m.find(node_name);
    if (it == m.end()) return false;
    if (it->second.has_type) out_type = it->second.type;  // else keep compiled type
    out_instance = it->second.instance;
    return true;
}

unsigned machine_instance_offset() noexcept {
    const char* mi = std::getenv("THEIA_MACHINE_INSTANCE");
    if (!mi || !*mi) return 0;
    return static_cast<unsigned>(std::strtoul(mi, nullptr, 10));
}

}  // namespace runtime
}  // namespace theia
