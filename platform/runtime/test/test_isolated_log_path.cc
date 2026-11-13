// isolated_log_path: an isolated stack (THEIA_PG_SUPERVISOR_INSTANCE=N, N != 0)
// writes <dir>/i<N>/<file>; the default stack keeps the path.
#include "Logger.hh"

#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
static void eq(const char* env, const std::string& in, const std::string& want) {
    if (env) setenv("THEIA_PG_SUPERVISOR_INSTANCE", env, 1); else unsetenv("THEIA_PG_SUPERVISOR_INSTANCE");
    const std::string got = theia::runtime::isolated_log_path(in);
    if (got != want) { std::printf("FAIL env=%s %s -> %s (want %s)\n", env ? env : "-", in.c_str(), got.c_str(), want.c_str()); ++fails; }
}

int main() {
    eq(nullptr, "/tmp/theia/truth.log", "/tmp/theia/truth.log");
    eq("0",     "/tmp/theia/truth.log", "/tmp/theia/truth.log");
    eq("x",     "/tmp/theia/truth.log", "/tmp/theia/truth.log");
    eq("2",     "/tmp/theia/truth.log", "/tmp/theia/i2/truth.log");
    eq("14",    "/var/log/sm.log",      "/var/log/i14/sm.log");
    eq("3",     "rel.log",              "./i3/rel.log");
    std::printf(fails ? "%d FAILED\n" : "isolated_log_path: ok\n", fails);
    return fails ? 1 : 0;
}
