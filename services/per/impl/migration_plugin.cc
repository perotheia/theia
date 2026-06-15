// Migration plugin loader (host half). dlopen a transform plugin and bridge its
// C edges into the C++ MigrationRegistry. See migration_plugin.hpp / _api.h.

#include "impl/migration_plugin.hpp"
#include "impl/migration_plugin_api.h"
#include "impl/migration_registry.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

// The opaque host context the C header forward-declares. No per-load state is
// needed beyond "register into the global registry", so it's an empty tag type.
// Defined at global scope to match the C `struct per_migration_host`.
struct per_migration_host {};

namespace ara::per {

namespace {

// Plugins loaded so far (by path) — keep the handles alive (the registered
// std::function captures the plugin's code; dlclose'ing would dangle it) and
// make re-load a no-op.
std::mutex g_mu;
std::set<std::string> g_loaded;

::per_migration_host g_host_ctx;

// add_edge: wrap the C per_transform_fn in a C++ Transform that runs it,
// owns the malloc'd output, and frees it. A transform that returns non-zero
// (failure) yields the input UNCHANGED — the bulk/read caller then sees the
// pre-transform bytes for that step; MigrateBulk treats a no-op chain as a
// reason to skip the value, GetConfig falls back to the stored value.
void host_add_edge(per_migration_host* /*host*/,
                   const char* from_digest, const char* to_digest,
                   per_transform_fn fn) {
    if (!from_digest || !to_digest || !fn) return;
    MigrationRegistry::instance().add(
        from_digest, to_digest,
        [fn](const std::string& in) -> std::string {
            char* out = nullptr;
            size_t out_len = 0;
            const int rc = fn(in.data(), in.size(), &out, &out_len);
            if (rc != 0 || out == nullptr) {
                std::free(out);
                return in;   // transform failed → identity (caller decides)
            }
            std::string result(out, out_len);
            std::free(out);
            return result;
        });
}

}  // namespace

bool load_migration_plugin(const std::string& so_path, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_loaded.count(so_path)) return true;   // already loaded
    }

    // RTLD_NOW: resolve all symbols up front so a broken plugin fails HERE, not
    // mid-migration. RTLD_LOCAL: the plugin's symbols don't leak into per's
    // global namespace. The handle is intentionally NOT dlclose'd (the
    // registered transforms call back into the plugin's code for the process
    // lifetime).
    void* h = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        // dlerror() CLEARS the error on read, so call it exactly ONCE — a second
        // call (e.g. in a `?:` that reads it twice) returns nullptr and
        // `std::string + nullptr` segfaults. Capture, then build the message.
        const char* de = ::dlerror();
        return fail(std::string("dlopen failed: ") + (de ? de : "unknown"));
    }

    auto entry = reinterpret_cast<per_register_migrations_fn>(
        ::dlsym(h, PER_MIGRATION_ENTRY_SYMBOL));
    if (!entry) {
        ::dlclose(h);
        return fail(std::string("missing entry symbol '") +
                    PER_MIGRATION_ENTRY_SYMBOL + "'");
    }

    per_migration_api api{};
    api.abi_version = PER_MIGRATION_ABI_VERSION;
    api.host = &g_host_ctx;
    api.add_edge = &host_add_edge;
    entry(&api);   // plugin registers its edges via host_add_edge

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_loaded.insert(so_path);
    }
    return true;
}

}  // namespace ara::per
