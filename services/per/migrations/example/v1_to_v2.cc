// Example config-migration PLUGIN — built as a standalone .so, dlopen'd by the
// running per at MigrateBulk (and feeding lazy migration-on-read).
//
// This is the n+1 side: the reshape code authored with the new schema version,
// shipped as a .so so the already-deployed (n) per can run it without being
// recompiled. It speaks ONLY the C ABI in migration_plugin_api.h — no per C++
// types — so it's compiler/version independent from the host.
//
// Real transforms decode the FROM-schema proto, copy fields forward into the
// TO-schema proto, and re-serialize. These two edges (v1->v2, v2->v3) are
// trivial byte appends so the host's chain (v1->v3 runs both) is testable.

#include "impl/migration_plugin_api.h"

#include <cstdlib>
#include <cstring>

namespace {

// Helper: emit `in` + `suffix` into a freshly malloc'd buffer the host frees.
int append(const char* in, size_t in_len, const char* suffix,
           char** out, size_t* out_len) {
    const size_t slen = std::strlen(suffix);
    char* buf = static_cast<char*>(std::malloc(in_len + slen));
    if (!buf) return 1;
    std::memcpy(buf, in, in_len);
    std::memcpy(buf + in_len, suffix, slen);
    *out = buf;
    *out_len = in_len + slen;
    return 0;
}

int v1_to_v2(const char* in, size_t in_len, char** out, size_t* out_len) {
    return append(in, in_len, "+v2", out, out_len);
}
int v2_to_v3(const char* in, size_t in_len, char** out, size_t* out_len) {
    return append(in, in_len, "+v3", out, out_len);
}

}  // namespace

// The required entry point: the host dlsym's PER_MIGRATION_ENTRY_SYMBOL and
// calls it; we register our adjacent edges. Chaining (v1->v3) is the host's job.
extern "C" void per_register_migrations(const per_migration_api* api) {
    if (!api || api->abi_version != PER_MIGRATION_ABI_VERSION) return;
    api->add_edge(api->host, "v1", "v2", &v1_to_v2);
    api->add_edge(api->host, "v2", "v3", &v2_to_v3);
}
