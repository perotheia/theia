// MachineInstance — see MachineInstance.hh.

#include "MachineInstance.hh"

#include <cstdlib>
#include <cstring>

namespace theia {
namespace runtime {

uint32_t machine_instance() noexcept {
    // Read + cache once. THEIA_MACHINE_INSTANCE is a small decimal (0,1,2,…);
    // anything unparseable or absent → 0.
    static const uint32_t kInstance = []() -> uint32_t {
        const char* e = std::getenv("THEIA_MACHINE_INSTANCE");
        if (!e || !*e) return 0u;
        char* end = nullptr;
        unsigned long v = std::strtoul(e, &end, 10);
        if (end == e) return 0u;  // not a number
        return static_cast<uint32_t>(v);
    }();
    return kInstance;
}

bool resolve_node_tipc(const char* node_name,
                       uint32_t default_type, uint32_t default_instance,
                       uint32_t& out_type, uint32_t& out_instance) noexcept {
    // Fallback: the compiled defaults, with this machine's index applied to the
    // instance (so even an env-less run with THEIA_MACHINE_INSTANCE set shifts).
    out_type     = default_type;
    out_instance = default_instance + machine_instance();

    const char* env = std::getenv("THEIA_NODE_TIPC");
    if (!env || !*env || !node_name) return false;

    // Format: "<node>=<type>:<inst>|<node2>=<type>:<inst>|...". Scan for our node.
    const size_t nlen = std::strlen(node_name);
    const char* p = env;
    while (*p) {
        // Start of an entry: match "<node_name>=".
        const char* eq = std::strchr(p, '=');
        if (!eq) break;
        const size_t this_nlen = static_cast<size_t>(eq - p);
        const char* val = eq + 1;
        const char* bar = std::strchr(val, '|');
        const char* val_end = bar ? bar : val + std::strlen(val);

        if (this_nlen == nlen && std::strncmp(p, node_name, nlen) == 0) {
            // val is "<type>:<inst>" (type may be hex "0x...."). strtoul base 0
            // auto-detects 0x. The instance here is ALREADY machine-shifted by
            // the supervisor, so don't re-apply machine_instance().
            char* cp = nullptr;
            unsigned long ty = std::strtoul(val, &cp, 0);
            if (cp && *cp == ':') {
                unsigned long in = std::strtoul(cp + 1, nullptr, 10);
                out_type     = static_cast<uint32_t>(ty);
                out_instance = static_cast<uint32_t>(in);
                return true;
            }
            return false;  // malformed entry → keep the fallback
        }
        if (!bar) break;
        p = bar + 1;
        (void)val_end;
    }
    return false;
}

}  // namespace runtime
}  // namespace theia
