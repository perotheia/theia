// Migration plugin loader — dlopen a version-n+1 transform plugin into the
// running per (version n) and register its edges into the MigrationRegistry.
//
// See migration_plugin_api.h for the why + the C ABI. This is the host half:
// dlopen the .so, dlsym the entry symbol, hand the plugin a host API whose
// add_edge bridges each C per_transform_fn into a C++ Transform stored in the
// registry. Idempotent per path (a .so loaded once stays loaded; re-loading the
// same path is a no-op). Returns true on success.

#pragma once

#include <string>

namespace system_services_per {

// Load the migration plugin at `so_path` and register its transform edges.
// On any failure (open / missing symbol / ABI mismatch) returns false and
// writes a reason to `err` (if non-null). Once loaded, the plugin's edges are
// visible to BOTH MigrateBulk and GetConfig's lazy migration-on-read.
bool load_migration_plugin(const std::string& so_path, std::string* err);

}  // namespace system_services_per
