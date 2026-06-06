/* Migration plugin ABI — the C contract between the running per (version n) and
 * a config-migration transform plugin shipped with version n+1.
 *
 * Why a plugin (dlopen) instead of compiled-in transforms: the n->n+1 reshape
 * code is authored WITH n+1, but it must run INSIDE the already-deployed per
 * (version n) — which obviously can't have it baked in. So n+1 ships a .so; the
 * running per dlopen's it at MigrateBulk time (and for lazy migration-on-read).
 * ONE plugin serves both paths: it registers its (from_digest -> to_digest)
 * transform edges, which feed both the bulk rewrite and GetConfig's read path.
 *
 * This is a pure-C ABI ON PURPOSE: the plugin is compiled by a DIFFERENT
 * toolchain/version than the host, so it must NOT touch per's C++ types (the
 * MigrationRegistry, std::string, etc.). The plugin only sees these C structs +
 * the host-provided register callback. Bump PER_MIGRATION_ABI_VERSION on any
 * incompatible change; the host rejects a mismatch.
 */
#ifndef PER_MIGRATION_PLUGIN_API_H
#define PER_MIGRATION_PLUGIN_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PER_MIGRATION_ABI_VERSION 1u

/* A transform reshapes config bytes from one schema digest to the next.
 * in/in_len: the value at the FROM schema. The plugin writes a malloc()'d
 * buffer to *out (the host free()s it) and its length to *out_len. Returns 0 on
 * success, non-zero on failure (the host aborts that value's migration). */
typedef int (*per_transform_fn)(const char* in, size_t in_len,
                                char** out, size_t* out_len);

/* Opaque host context handed back to the register callback. */
typedef struct per_migration_host per_migration_host;

/* The host passes this to the plugin's entry point. The plugin calls add_edge
 * once per adjacent (from->to) step it provides; the host wires each into its
 * MigrationRegistry. Chaining (v1->v3 via v1->v2,v2->v3) is the host's job. */
typedef struct {
    unsigned abi_version;          /* = PER_MIGRATION_ABI_VERSION (host fills) */
    per_migration_host* host;      /* pass back to add_edge */
    void (*add_edge)(per_migration_host* host,
                     const char* from_digest, const char* to_digest,
                     per_transform_fn fn);
} per_migration_api;

/* The plugin's REQUIRED entry point. The host dlsym()s this exact name after
 * dlopen, then calls it; the plugin registers all its edges via api->add_edge.
 *   extern "C" void per_register_migrations(const per_migration_api* api);
 */
#define PER_MIGRATION_ENTRY_SYMBOL "per_register_migrations"
typedef void (*per_register_migrations_fn)(const per_migration_api* api);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PER_MIGRATION_PLUGIN_API_H */
