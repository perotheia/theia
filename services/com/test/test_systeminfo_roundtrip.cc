// Regression gate for the com GetSystemInfo nanopb→C++ cross-format parse.
//
// com's SupLink::get_system_info re-encodes the supervisor's nanopb SystemInfo
// reply to bytes; cache_machine_sysinfo + ListMachines then PARSE those bytes with
// the libprotobuf (C++) SystemInfo and read machine_name (tag 14) / release_version
// (tag 15). A bug dropped those two HIGH-TAG STATIC char[] string fields on the
// round-trip (ListMachines reported "m0" + empty release), surfacing as a Heisenbug
// (a -O2 read-elision that an fprintf "fixed"). This test pins the cross-format
// contract end-to-end, in-process, so CI catches a regression of EITHER:
//   1. nanopb encode/decode of the high-tag fields, and
//   2. the nanopb-bytes → C++ libprotobuf ParseFromString of the same.
//
// No stack, no threads — pure encode/parse. Fast, hermetic, always-on.

#include <cstdio>
#include <cstring>
#include <string>

#include <pb_encode.h>
#include <pb_decode.h>
#include "system/supervisor/supervisor.pb.h"       // nanopb
#include "cpp/supervisor.pb.h"                       // libprotobuf (C++)

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    // ── Build a SystemInfo the way the supervisor does (all fields incl the two
    //    high-tag ones), nanopb-encode it. ────────────────────────────────────
    system_supervisor_SystemInfo src = system_supervisor_SystemInfo_init_zero;
    std::snprintf(src.hostname, sizeof(src.hostname), "raspberrypi");
    std::snprintf(src.kernel, sizeof(src.kernel), "Linux 6.8.0 aarch64");
    std::snprintf(src.os_pretty_name, sizeof(src.os_pretty_name),
                  "Debian GNU/Linux 12 (bookworm)");
    std::snprintf(src.theia_git_sha, sizeof(src.theia_git_sha), "abcdef1234567890");
    std::snprintf(src.build_timestamp, sizeof(src.build_timestamp), "2026-06-28T19:00");
    std::snprintf(src.machine_name, sizeof(src.machine_name), "central");
    std::snprintf(src.release_version, sizeof(src.release_version),
                  "0.2.4-bookworm-arm64");
    src.start_timestamp_ms = 12345;

    uint8_t buf[2048];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, system_supervisor_SystemInfo_fields, &src),
          "nanopb encode SystemInfo");
    const std::string wire(reinterpret_cast<const char*>(buf), os.bytes_written);

    // ── 1. nanopb DECODE round-trip (the SupLink reply path) — the high-tag
    //    char[] fields must survive. ───────────────────────────────────────────
    {
        system_supervisor_SystemInfo dst = system_supervisor_SystemInfo_init_zero;
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CHECK(pb_decode(&is, system_supervisor_SystemInfo_fields, &dst),
              "nanopb decode SystemInfo");
        CHECK(std::strcmp(dst.machine_name, "central") == 0,
              "nanopb decode preserves machine_name (tag 14)");
        CHECK(std::strcmp(dst.release_version, "0.2.4-bookworm-arm64") == 0,
              "nanopb decode preserves release_version (tag 15)");
    }

    // ── 2. The ACTUAL bug path: parse the nanopb wire with the C++ libprotobuf
    //    SystemInfo (what cache_machine_sysinfo + ListMachines do). ────────────
    {
        system_supervisor::SystemInfo si;
        CHECK(si.ParseFromString(wire),
              "C++ ParseFromString of nanopb-encoded SystemInfo");
        CHECK(si.machine_name() == "central",
              "C++ parse preserves machine_name (tag 14) — the m0 bug");
        CHECK(si.release_version() == "0.2.4-bookworm-arm64",
              "C++ parse preserves release_version (tag 15) — the stateless-base bug");
        // lower-tag fields too, for completeness
        CHECK(si.hostname() == "raspberrypi", "C++ parse hostname");
        CHECK(si.theia_git_sha() == "abcdef1234567890", "C++ parse git_sha");
    }

    if (failures == 0) std::printf("ALL SYSTEMINFO ROUNDTRIP CHECKS PASSED\n");
    return failures == 0 ? 0 : 1;
}
