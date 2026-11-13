// The config keyspace per isolated stack (etcd_store.hpp): the default stack
// keeps /theia/config/, `theia start --instance N` (N != 0) gets /theia/i<N>/config/.
#include <cstdio>
#include <string>
#include "../impl/etcd_store.hpp"

using ara::per::config_prefix_for;

static int fails = 0;
static void eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) { std::printf("FAIL %s: got '%s' want '%s'\n", what, got.c_str(), want.c_str()); ++fails; }
}

int main() {
    eq(config_prefix_for(nullptr), "/theia/config/", "unset");
    eq(config_prefix_for(""),      "/theia/config/", "empty");
    eq(config_prefix_for("0"),     "/theia/config/", "instance 0");
    eq(config_prefix_for("x1"),    "/theia/config/", "garbage");
    eq(config_prefix_for("2x"),    "/theia/config/", "trailing garbage");
    eq(config_prefix_for("1"),     "/theia/i1/config/", "instance 1");
    eq(config_prefix_for("3"),     "/theia/i3/config/", "instance 3");
    // No isolated prefix is a prefix of the default one or of another stack's.
    const std::string d = config_prefix_for("0"), a = config_prefix_for("1"), b = config_prefix_for("12");
    if (a.compare(0, d.size(), d) == 0 || b.compare(0, a.size(), a) == 0) { std::printf("FAIL overlap\n"); ++fails; }
    std::printf(fails ? "%d FAILED\n" : "config prefix: ok\n", fails);
    return fails ? 1 : 0;
}
