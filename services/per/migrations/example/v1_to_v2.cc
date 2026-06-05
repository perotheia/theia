// Example config migration transforms — proves the lazy-migration chain.
//
// Real migrations decode the config bytes with the OLD schema's proto, copy the
// fields forward into the NEW schema's proto, and re-serialize. Since config
// values are opaque to per, a transform is just bytes -> bytes; only the
// migration author (who owns both schemas) knows how to walk them. These two
// edges (v1->v2, v2->v3) are deliberately trivial so the BFS chain (v1->v3 runs
// both, in order) is testable without a real proto.
//
// Registered at static-init via MigrationRegistrar (the trace_decoder_protos
// pattern). A file like this lives at
// services/per/migrations/<config_type>/<from>_to_<to>.cc.

#include "impl/migration_registry.hpp"

#include <string>

namespace system_services_per {
namespace {

// v1 -> v2: append "+v2" (stand-in for "add field X with a default").
[[maybe_unused]] const MigrationRegistrar kV1toV2{
    "v1", "v2",
    [](const std::string& bytes) { return bytes + "+v2"; }};

// v2 -> v3: append "+v3" (stand-in for "rename / reshape field Y").
[[maybe_unused]] const MigrationRegistrar kV2toV3{
    "v2", "v3",
    [](const std::string& bytes) { return bytes + "+v3"; }};

}  // namespace
}  // namespace system_services_per
