// mender_install — see mender_install.hpp. The standalone-install back-end.

#include "impl/mender_install.hpp"

#include <array>
#include <cstdio>
#include <memory>

namespace ara::ucm {

namespace {
// Run a shell command, capture combined output + exit code. (popen — the install
// is an out-of-process delegate; UCM stays the lifecycle owner.)
int run_capture(const std::string& cmd, std::string& out) {
    out.clear();
    std::string full = cmd + " 2>&1";
    FILE* p = ::popen(full.c_str(), "r");
    if (!p) return -1;
    std::array<char, 512> buf;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0)
        out.append(buf.data(), n);
    int rc = ::pclose(p);
    // pclose returns the wait status; extract the exit code.
    if (rc == -1) return -1;
    return (rc & 0x7f) == 0 ? ((rc >> 8) & 0xff) : 128 + (rc & 0x7f);
}

bool have_mender() {
    std::string out;
    // `command -v` is 0 iff a mender CLI is on PATH.
    return run_capture("command -v mender mender-update", out) == 0 && !out.empty();
}
}  // namespace

InstallResult mender_standalone_install(const std::string& artifact_url) {
    InstallResult r;
    const char* be = std::getenv("THEIA_UCM_MENDER");
    const std::string backend = (be && *be) ? be : "mender";

    if (backend == "simulate" || !have_mender()) {
        r.ok = true;
        r.rc = 0;
        r.detail = "simulate (role artifact assumed pre-staged): " + artifact_url;
        return r;
    }

    // Standalone install — the CLI verb differs across Mender versions:
    //   4.x:  mender-update install <url>
    //   3.x:  mender install <url>          (subcommand)
    //   2.x:  mender -install <url>         (flag)
    // Try them in order; the first that exists + succeeds wins. The theia-release
    // update module writes the bits + flips current; UCM then runs its
    // PROVISIONAL/restart/verify lifecycle over the result.
    std::string out;
    std::string q = "'" + artifact_url + "'";
    std::string cmd =
        "mender-update install " + q +
        " || mender install " + q +
        " || mender -install " + q;
    r.rc = run_capture(cmd, out);
    r.ok = (r.rc == 0);
    r.detail = (r.ok ? "mender install ok: " : "mender install FAILED: ") +
               artifact_url + (out.empty() ? "" : (" — " + out.substr(0, 200)));
    return r;
}

}  // namespace ara::ucm
