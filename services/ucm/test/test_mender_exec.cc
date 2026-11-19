// End-to-end check of the Mender back-end's exec path (impl/mender_install.cc)
// against a STUB `mender` on PATH: the artifact reference reaches the CLI as
// exactly one argv element, no shell interprets it, and an invalid reference is
// refused before anything is executed.

#include "impl/mender_install.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace ara::ucm;

static int g_fail = 0, g_run = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        ++g_run;                                                              \
        if (!(cond)) {                                                        \
            ++g_fail;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

int main() {
    const char* t = std::getenv("TEST_TMPDIR");
    char tmpl[] = "/tmp/ucm_mender_exec.XXXXXX";
    const std::string dir = (t && *t) ? std::string(t) : std::string(::mkdtemp(tmpl));
    const std::string bin = dir + "/bin";
    ::mkdir(bin.c_str(), 0755);
    const std::string log = dir + "/argv.log";
    const std::string pwned = dir + "/PWNED";

    // The stub: one line per argv element, then the exit code from $STUB_RC.
    {
        std::ofstream f(bin + "/mender");
        f << "#!/bin/sh\n"
             "echo \"argc=$#\" >> \"" << log << "\"\n"
             "for a in \"$@\"; do printf 'arg=%s\\n' \"$a\" >> \"" << log << "\"; done\n"
             "echo stub-output\n"
             "exit \"${STUB_RC:-0}\"\n";
    }
    ::chmod((bin + "/mender").c_str(), 0755);
    // PATH = the stub dir + the system dirs (the stub needs /bin/sh's tools);
    // there is no mender-update, so the 4.x attempt fails with 127 and the 3.x
    // form `mender install <ref>` is what must succeed.
    ::setenv("PATH", (bin + ":/usr/bin:/bin").c_str(), 1);
    ::setenv("THEIA_UCM_MENDER", "mender", 1);
    ::setenv("THEIA_UCM_ARTIFACT_DIRS", dir.c_str(), 1);

    // 1. a valid local reference is exec'd, as ONE argument
    {
        const std::string ref = dir + "/bundle/central.mender";
        ::setenv("STUB_RC", "0", 1);
        const InstallResult r = mender_standalone_install(ref);
        EXPECT(r.ok && r.rc == 0);
        EXPECT(r.detail.find("stub-output") != std::string::npos);   // output captured
        EXPECT(slurp(log) == "argc=2\narg=install\narg=" + ref + "\n");
    }
    // 2. a valid https reference with query characters stays one argument
    {
        ::unlink(log.c_str());
        const std::string ref = "https://gs.example.org:9000/r/0.2.2/central.mender?sig=a%2Fb&exp=600";
        const InstallResult r = mender_standalone_install(ref);
        EXPECT(r.ok);
        EXPECT(slurp(log) == "argc=2\narg=install\narg=" + ref + "\n");
    }
    // 3. a failing CLI is reported (and the 2.x form is tried after the 3.x one)
    {
        ::unlink(log.c_str());
        ::setenv("STUB_RC", "7", 1);
        const std::string ref = dir + "/bundle/central.mender";
        const InstallResult r = mender_standalone_install(ref);
        EXPECT(!r.ok && r.rc == 7);
        EXPECT(slurp(log) == "argc=2\narg=install\narg=" + ref + "\n"
                             "argc=2\narg=-install\narg=" + ref + "\n");
        ::setenv("STUB_RC", "0", 1);
    }
    // 4. injection attempts: refused, NOTHING executed, no side effect
    for (const std::string ref : {
             "x'; touch " + pwned + "; echo '",                       // the popen-era break-out
             dir + "/a.mender'; touch " + pwned + "; echo '",
             "https://host/a.mender'; touch " + pwned + " #",
             "https://host/$(touch " + pwned + ")",
             "https://host/`touch " + pwned + "`",
             dir + "/../../etc/passwd",
             std::string("/etc/passwd"),
             std::string("central.mender"),
             std::string("s3://theia-roles/rig/0.2.2/central.mender"),
             std::string("--help")}) {
        ::unlink(log.c_str());
        const InstallResult r = mender_standalone_install(ref);
        EXPECT(!r.ok);
        EXPECT(r.detail.find("REFUSED") != std::string::npos);
        EXPECT(!exists(log));          // the CLI was never run
        EXPECT(!exists(pwned));        // and no shell ran the payload
    }
    // 5. simulate back-end is unchanged: succeeds without running or validating
    {
        ::unlink(log.c_str());
        ::setenv("THEIA_UCM_MENDER", "simulate", 1);
        const InstallResult r = mender_standalone_install("theia.mender");
        EXPECT(r.ok && r.rc == 0 && !exists(log));
        ::setenv("THEIA_UCM_MENDER", "mender", 1);
    }
    // 6. no mender on PATH → simulate (unchanged)
    {
        ::setenv("PATH", (dir + "/nowhere").c_str(), 1);
        const InstallResult r = mender_standalone_install("theia.mender");
        EXPECT(r.ok && r.rc == 0 && !exists(log));
    }

    std::printf("test_mender_exec: %d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
