// mender_install — see mender_install.hpp. The standalone-install back-end.

#include "impl/mender_install.hpp"
#include "impl/ucm_validate.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ara::ucm {

namespace {
// Run argv[0] (resolved on PATH) with argv, NO SHELL: fork + execvp. Appends the
// child's combined stdout+stderr to `out`; returns its exit code (127 = could
// not exec, 128+N = killed by signal N, -1 = fork/pipe failure). The install is
// an out-of-process delegate; UCM stays the lifecycle owner.
int run_capture(const std::vector<std::string>& argv, std::string& out) {
    if (argv.empty()) return -1;
    std::vector<char*> av;
    av.reserve(argv.size() + 1);
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return -1; }
    if (pid == 0) {
        ::close(fds[0]);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::close(devnull); }
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::execvp(av[0], av.data());
        ::_exit(127);                       // not found / not executable
    }
    ::close(fds[1]);
    std::array<char, 512> buf;
    for (;;) {
        const ssize_t n = ::read(fds[0], buf.data(), buf.size());
        if (n > 0) { out.append(buf.data(), static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fds[0]);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

// Is `name` an executable on PATH? (No shell: walk PATH + access(X_OK).)
bool on_path(const char* name) {
    const char* p = std::getenv("PATH");
    const std::string path = (p && *p) ? p : "/usr/local/bin:/usr/bin:/bin";
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        const std::string dir = (j == i) ? "." : path.substr(i, j - i);
        if (::access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        i = j + 1;
    }
    return false;
}

bool have_mender() { return on_path("mender") || on_path("mender-update"); }
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

    // The reference comes off the wire (manifest artifact_path): only an absolute
    // local path under an allowed directory or an https:// URL reaches the CLI
    // (impl/ucm_validate.hpp). Anything else fails the install (fail-closed).
    std::string why;
    if (!valid_artifact_ref(artifact_url, allowed_artifact_dirs(), &why)) {
        r.ok = false;
        r.rc = -1;
        r.detail = "mender install REFUSED — invalid artifact reference (" + why +
                   "); allowed: an absolute path under THEIA_UCM_ARTIFACT_DIRS "
                   "(default /opt/theia:/var/lib/theia) or an https:// URL";
        return r;
    }

    // Standalone install — the CLI verb differs across Mender versions:
    //   4.x:  mender-update install <url>
    //   3.x:  mender install <url>          (subcommand)
    //   2.x:  mender -install <url>         (flag)
    // Try them in order; the first that exists + succeeds wins. The theia-release
    // update module writes the bits + flips current; UCM then runs its
    // PROVISIONAL/restart/verify lifecycle over the result. Each is exec'd with
    // an argv vector — the reference is ONE argument, never parsed by a shell.
    std::string out;
    for (const auto& argv : mender_install_argvs(artifact_url)) {
        r.rc = run_capture(argv, out);
        if (r.rc == 0) break;
    }
    r.ok = (r.rc == 0);
    r.detail = (r.ok ? "mender install ok: " : "mender install FAILED: ") +
               artifact_url + (out.empty() ? "" : (" — " + out.substr(0, 200)));
    return r;
}

}  // namespace ara::ucm
