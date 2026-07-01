// Pre-main environment check for the supervisor binary.
//
// This TU exists ONLY to host the [[gnu::constructor]] that validates
// THEIA_INSTALL_DIR BEFORE main() runs — and therefore before any TIPC socket
// is opened. Without this the supervisor binds TIPC, prints its "up" lines,
// and only then throws (from do_start()), giving the operator a confusing delay
// and misleading output.
//
// THEIA_SUPERVISOR_MANIFEST is intentionally NOT checked here: the manifest
// path can also be supplied as a positional CLI arg, which is not yet parsed
// when the constructor runs. manifest_path() in SupervisorWorker_handlers.cc
// handles the missing-manifest case after arg parsing.

#include <cstdio>
#include <cstdlib>

namespace {

[[gnu::constructor]]
void check_supervisor_env() {
    const char* install_dir = std::getenv("THEIA_INSTALL_DIR");
    if (!install_dir || !install_dir[0]) {
        std::fputs(
            "FATAL: THEIA_INSTALL_DIR is not set.\n"
            "  The supervisor requires THEIA_INSTALL_DIR — a colon-separated\n"
            "  list of directories to search when resolving child binaries.\n"
            "  Examples:\n"
            "    deb deploy : THEIA_INSTALL_DIR=/opt/theia/current\n"
            "    local dev  : THEIA_INSTALL_DIR=/opt/theia:/path/to/install/central\n"
            "  The launcher (theia start / theia-run.sh) exports it.\n",
            stderr);
        std::_Exit(2);
    }
}

}  // namespace
