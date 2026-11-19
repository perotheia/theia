// Unit check for the UCM manifest validators (impl/ucm_validate.hpp): the
// version shape, the two allowed artifact-reference forms, and that the argv
// built for the Mender CLI carries the reference as exactly ONE argument.

#include "impl/ucm_validate.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

int main() {
    // ---- version -----------------------------------------------------------
    for (const char* v : {"1.2.0", "0.2.2", "runtime-0.2.2", "demo-1.0.0-jammy",
                          "a", "A1", "1.0+build.7", "v1_", "perception-0.2.2-bookworm-arm64"})
        EXPECT(valid_version(v));
    EXPECT(valid_version(std::string(64, 'a')));
    EXPECT(!valid_version(std::string(65, 'a')));
    for (const char* v : {"", ".", "..", "../..", "../../etc", "a/b", "/etc", "a/../b",
                          ".hidden", "-rf", "+1", "_x", "a b", "a\nb", "a;b", "a$b",
                          "a`id`", "a'b", "a\"b", "a\\b", "a*", "1.0\n", " 1.0", "a|b",
                          "a&b", "a(b)", "a~b", "\xc3\xa9"})
        EXPECT(!valid_version(v));
    EXPECT(!valid_version(std::string("1.0\0x", 5)));     // embedded NUL

    // ---- local artifact paths ----------------------------------------------
    const std::vector<std::string> dirs = {"/opt/theia", "/var/lib/theia"};
    std::string why;
    EXPECT(valid_artifact_ref("/opt/theia/releases/1.2.0/central.mender", dirs, &why));
    EXPECT(valid_artifact_ref("/var/lib/theia/bundle/compute.mender", dirs));
    EXPECT(valid_artifact_ref("/opt/theia/a.mender", dirs));
    for (const char* p : {
             "/opt/theia",                           // the dir itself, not under it
             "/opt/theia/",                          // empty component
             "/opt/theiax/a.mender",                 // prefix, not a component boundary
             "/etc/passwd",
             "/tmp/a.mender",
             "/opt/theia/../../etc/passwd",          // traversal
             "/opt/theia/releases/../../../etc/x",
             "/opt/theia/./a.mender",
             "/opt/theia//a.mender",
             "/opt/theia/a b.mender",
             "/opt/theia/a;id.mender",
             "/opt/theia/$(id).mender",
             "/opt/theia/`id`.mender",
             "/opt/theia/a'.mender",
             "/opt/theia/a\".mender",
             "/opt/theia/a\\b.mender",
             "/opt/theia/a|b",
             "/opt/theia/a&b",
             "/opt/theia/a>b",
             "/opt/theia/a\nb",
             "/opt/theia/a*",
             "/"}) {
        why.clear();
        EXPECT(!valid_artifact_ref(p, dirs, &why));
        EXPECT(!why.empty());
    }
    EXPECT(!valid_artifact_ref("/opt/theia/a.mender", {}));          // no allowed dir
    EXPECT(valid_artifact_ref("/srv/ota/a.mender", {"/srv/ota"}));   // custom allow-list
    EXPECT(!valid_artifact_ref("/opt/theia/a.mender", {"/srv/ota"}));

    // ---- https URLs --------------------------------------------------------
    for (const char* u : {
             "https://gs.example.org/roles/theia-rig/0.2.2/central.mender",
             "https://gs.example.org:9000/theia-roles/rig/0.2.2/compute.mender",
             "https://10.0.0.5/a.mender?X-Amz-Signature=abc123&X-Amz-Expires=600",
             "https://host/a%20b.mender",
             "https://host"})
        EXPECT(valid_artifact_ref(u, dirs));
    for (const char* u : {
             "http://gs.example.org/a.mender",        // not https
             "HTTPS://gs.example.org/a.mender",
             "file:///etc/passwd",
             "s3://theia-roles/rig/0.2.2/central.mender",
             "ftp://host/a.mender",
             "https://",                              // no host
             "https:///a.mender",
             "https://host/../a.mender",
             "https://host/a/../../b.mender",
             "https://host/a.mender'; rm -rf /; echo '",   // the popen-era injection
             "https://host/a.mender; id",
             "https://host/a.mender|id",
             "https://host/a.mender && id",
             "https://host/$(id).mender",
             "https://host/`id`.mender",
             "https://host/a\".mender",
             "https://host/a\\b.mender",
             "https://host/a.mender\nmender",
             "https://host/a.mender\t",
             "https://host/a b.mender",
             "https://host/a>b",
             "https://host/a<b",
             "https://host/a(b)",
             "https://host/a#frag",
             "https://-host/a.mender"}) {
        why.clear();
        EXPECT(!valid_artifact_ref(u, dirs, &why));
        EXPECT(!why.empty());
    }

    // ---- everything else ---------------------------------------------------
    for (const char* r : {"", "central.mender", "./central.mender", "../central.mender",
                          "theia.mender", "-install", "--help", "~/a.mender",
                          "'/opt/theia/a.mender'", " /opt/theia/a.mender"})
        EXPECT(!valid_artifact_ref(r, dirs));
    EXPECT(!valid_artifact_ref("https://host/" + std::string(600, 'a'), dirs));
    EXPECT(!valid_artifact_ref(std::string("/opt/theia/a\0b", 14), dirs));   // embedded NUL

    // ---- allowed dirs from the environment ---------------------------------
    ::unsetenv("THEIA_UCM_ARTIFACT_DIRS");
    {
        const auto d = allowed_artifact_dirs();
        EXPECT(d.size() == 2 && d[0] == "/opt/theia" && d[1] == "/var/lib/theia");
    }
    ::setenv("THEIA_UCM_ARTIFACT_DIRS", "/srv/ota/:relative:/:/data/art", 1);
    {
        const auto d = allowed_artifact_dirs();     // "/" and relative entries dropped
        EXPECT(d.size() == 2 && d[0] == "/srv/ota" && d[1] == "/data/art");
        EXPECT(valid_artifact_ref("/srv/ota/x.mender", d));
        EXPECT(!valid_artifact_ref("/etc/x.mender", d));
    }
    ::unsetenv("THEIA_UCM_ARTIFACT_DIRS");

    // ---- the Mender argv: the reference is ONE argument --------------------
    {
        // Deliberately hostile: argv building must not split or interpret it
        // (validation is a separate, earlier step).
        const std::string ref = "https://host/a.mender'; rm -rf / #  $(id) `id` | x";
        const auto argvs = mender_install_argvs(ref);
        EXPECT(argvs.size() == 3);
        EXPECT(argvs[0] == (std::vector<std::string>{"mender-update", "install", ref}));
        EXPECT(argvs[1] == (std::vector<std::string>{"mender", "install", ref}));
        EXPECT(argvs[2] == (std::vector<std::string>{"mender", "-install", ref}));
        for (const auto& a : argvs) {
            EXPECT(a.size() == 3);                 // program, verb, ONE reference
            EXPECT(a.back() == ref);
            int hits = 0;
            for (const auto& e : a) if (e.find("host") != std::string::npos) ++hits;
            EXPECT(hits == 1);
            EXPECT(a[0].find(' ') == std::string::npos && a[0] != "sh" && a[0] != "/bin/sh");
        }
    }

    std::printf("test_ucm_validate: %d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
