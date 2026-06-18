// gps_rtk — the RTK GNSS variant (`--define gps=rtk`). The high-precision path
// for a u-blox ZED-F9R (the odd-path-monitor rig's receiver — see
// up/odd-path-monitor/docs/rtk.md): it reads the binary UBX protocol off
// /dev/serial0 and parses UBX-NAV-PVT (Navigation Position Velocity Time), which
// carries UTC + lat/lon/alt + the RTK carrier-solution status in ONE frame.
//
// UBX framing (header-only parse, same spirit as services/diag/impl/doip.hpp):
//   B5 62 | class | id | len_lo len_hi | payload[len] | ck_a ck_b
//   NAV-PVT = class 0x01 id 0x07, payload 92 bytes. We use:
//     off 0   U4 iTOW
//     off 4   U2 year, 6 U1 month, 7 U1 day, 8 U1 hour, 9 U1 min, 10 U1 sec
//     off 11  X1 valid  (bit0 validDate, bit1 validTime, bit2 fullyResolved)
//     off 16  I4 nano   (ns, signed, sub-second correction)
//     off 20  U1 fixType (3 = 3D)
//     off 21  X1 flags  (bits6..7 carrSoln: 1=float, 2=fixed → rtk_fix)
//     off 24  I4 lon (1e-7 deg), 28 I4 lat (1e-7 deg), 36 I4 hMSL (mm)
//
// Open-read-parse-close per poll (v1); a real high-rate path holds the fd open
// and runs a streaming parser. The checksum is validated (an 8-bit Fletcher).

#include "impl/gps_backend.hpp"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace ara::tsync {

namespace {

constexpr const char* kDefaultDev = "/dev/serial0";

uint16_t rd_u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
int32_t  rd_i32(const uint8_t* p) {
    return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// UBX 8-bit Fletcher checksum over class..payload (the bytes between the two
// sync chars and the checksum).
bool ubx_cksum_ok(const uint8_t* frame, size_t body_len) {
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 2 + body_len; ++i) { a += frame[i]; b += a; }
    return frame[2 + body_len] == a && frame[2 + body_len + 1] == b;
}

// Decode the UTC fields of a NAV-PVT payload → epoch ns (0 if time not resolved).
uint64_t navpvt_utc_ns(const uint8_t* pl) {
    uint8_t valid = pl[11];
    if (!(valid & 0x04)) return 0;   // not fullyResolved
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = rd_u16(pl + 4) - 1900;
    tmv.tm_mon  = pl[6] - 1;
    tmv.tm_mday = pl[7];
    tmv.tm_hour = pl[8];
    tmv.tm_min  = pl[9];
    tmv.tm_sec  = pl[10];
    time_t secs = ::timegm(&tmv);
    if (secs == (time_t)-1) return 0;
    int32_t nano = rd_i32(pl + 16);   // signed sub-second correction
    int64_t ns = static_cast<int64_t>(secs) * 1000000000LL + nano;
    return ns < 0 ? 0 : static_cast<uint64_t>(ns);
}

}  // namespace

GnssFix GpsBackend::poll(const std::string& dev_in) {
    GnssFix f;
    const std::string dev = dev_in.empty() ? kDefaultDev : dev_in;

    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) { f.note = "rtk: open(" + dev + ") failed"; return f; }

    std::vector<uint8_t> buf;
    uint8_t rd[2048];
    for (int i = 0; i < 16; ++i) {
        ssize_t r = ::read(fd, rd, sizeof(rd));
        if (r <= 0) break;
        buf.insert(buf.end(), rd, rd + r);
        if (buf.size() > 65536) break;
    }
    ::close(fd);
    if (buf.empty()) { f.note = "rtk: no UBX data on " + dev; return f; }

    // Scan for the most recent valid NAV-PVT frame.
    const uint8_t* found = nullptr;
    for (size_t i = 0; i + 8 <= buf.size(); ++i) {
        if (buf[i] != 0xB5 || buf[i + 1] != 0x62) continue;
        uint8_t cls = buf[i + 2], id = buf[i + 3];
        uint16_t len = rd_u16(&buf[i + 4]);
        size_t total = 6 + len + 2;               // sync..checksum
        if (i + total > buf.size()) continue;      // truncated tail
        if (cls != 0x01 || id != 0x07 || len < 92) continue;
        if (!ubx_cksum_ok(&buf[i], 4 + len)) continue;  // body = class..payload
        found = &buf[i + 6];                        // payload start
        // keep scanning — take the LAST valid frame this poll
    }

    if (!found) { f.note = "rtk: no valid NAV-PVT on " + dev; return f; }

    uint64_t utc = navpvt_utc_ns(found);
    uint8_t fixType = found[20];
    uint8_t flags   = found[21];
    uint8_t carrSoln = (flags >> 6) & 0x03;       // 0 none, 1 float, 2 fixed
    f.lon = rd_i32(found + 24) * 1e-7;
    f.lat = rd_i32(found + 28) * 1e-7;
    f.alt = rd_i32(found + 36) * 1e-3;            // hMSL mm → m
    f.rtk_fix = (carrSoln != 0);
    if (utc && fixType >= 2) {
        f.valid = true; f.utc_ns = utc;
        f.note = std::string("rtk fix type=") + std::to_string(fixType) +
                 (carrSoln == 2 ? " RTK-fixed" : carrSoln == 1 ? " RTK-float" : "");
    } else {
        f.note = "rtk: NAV-PVT no time/fix yet";
    }
    return f;
}

const char* GpsBackend::name() { return "rtk"; }

}  // namespace ara::tsync
