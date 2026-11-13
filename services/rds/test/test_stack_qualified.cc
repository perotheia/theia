// stack_qualified: an isolated stack (THEIA_PG_SUPERVISOR_INSTANCE=N, N != 0)
// qualifies its iceoryx ids "<id>@<N>" (runtime name, stream service); the
// default stack and a rig (unset / 0 / not a number) keep the bare id.
// No RouDi needed.
#include "ara/rds/transport_if.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
static void eq(const char* env, const std::string& in, const std::string& want) {
    if (env) setenv("THEIA_PG_SUPERVISOR_INSTANCE", env, 1); else unsetenv("THEIA_PG_SUPERVISOR_INSTANCE");
    const std::string got = ara::rds::stack_qualified(in);
    if (got != want) { std::printf("FAIL env=%s %s -> %s (want %s)\n", env ? env : "-", in.c_str(), got.c_str(), want.c_str()); ++fails; }
}

int main() {
    eq(nullptr, "Cam_left", "Cam_left");
    eq("0",     "Cam_left", "Cam_left");
    eq("",      "Cam_left", "Cam_left");
    eq("x",     "Cam_left", "Cam_left");
    eq("3x",    "Cam_left", "Cam_left");
    eq("3",     "Cam_left", "Cam_left@3");
    eq("12",    "Spr_producer", "Spr_producer@12");
    // the logical name and the resolved triple never change; only connect qualifies
    setenv("THEIA_PG_SUPERVISOR_INSTANCE", "3", 1);
    const auto s = ara::rds::resolve_instance("/Cam_left/frames");
    if (s.service != "Cam_left" || s.instance != "frames") { std::printf("FAIL resolve_instance changed\n"); ++fails; }
    std::printf(fails ? "%d FAILED\n" : "stack_qualified: ok\n", fails);
    return fails ? 1 : 0;
}
