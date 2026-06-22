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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace ara::tsync {

namespace {

constexpr const char* kDefaultDev = "/dev/serial0";

// The ZED-F9P UART1 baud the receiver is configured for (SparkFun default that
// we set in CFG-UART1-BAUDRATE). Overridable at runtime via THEIA_GPS_BAUD so a
// re-flashed receiver doesn't need a rebuild. The earlier driver assumed the
// port was pre-set; on a fresh /dev/serial0 it defaults to 9600 → garbage, so
// we set the line discipline here.
constexpr speed_t kDefaultBaud = B38400;

speed_t baud_constant(long b) {
    switch (b) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return 0;   // unknown → caller keeps kDefaultBaud
    }
}

// Put the serial fd into raw 8N1 at the configured baud. No-op (returns true)
// on a non-tty (e.g. a regression test feeding a regular file). Best-effort:
// failure to set termios is logged via the fix note, not fatal.
bool configure_serial(int fd) {
    if (!::isatty(fd)) return true;   // file/pipe under test — nothing to set
    struct termios tio;
    if (::tcgetattr(fd, &tio) != 0) return false;
    ::cfmakeraw(&tio);
    speed_t baud = kDefaultBaud;
    if (const char* env = std::getenv("THEIA_GPS_BAUD")) {
        speed_t c = baud_constant(std::strtol(env, nullptr, 10));
        if (c) baud = c;
    }
    ::cfsetispeed(&tio, baud);
    ::cfsetospeed(&tio, baud);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;     // non-blocking read (we poll with O_NONBLOCK too)
    tio.c_cc[VTIME] = 0;
    return ::tcsetattr(fd, TCSANOW, &tio) == 0;
}

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

    // O_RDWR (not O_RDONLY): tcsetattr needs write access to the tty to set the
    // baud/line discipline. O_NOCTTY so the serial port never becomes our
    // controlling terminal.
    int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { f.note = "rtk: open(" + dev + ") failed"; return f; }
    if (!configure_serial(fd)) {
        f.note = "rtk: tcsetattr(" + dev + ") failed (baud not set)";
        // keep going — a pre-configured port may still yield data
    }

    // Collect ~one nav cycle of bytes. The receiver emits NAV-PVT (+NAV-ATT) at
    // the nav rate (1 Hz default), so at the instant we open the port there may
    // be NOTHING buffered yet — a plain non-blocking read loop would return 0 and
    // give "no UBX data". Wait with poll() up to a budget (>1 nav period) and
    // drain whatever arrives, so a full frame lands every poll.
    std::vector<uint8_t> buf;
    uint8_t rd[2048];
    const int budget_ms = 1500;   // > one 1 Hz period; covers PVT+ATT of a cycle
    struct pollfd pfd { fd, POLLIN, 0 };
    int waited = 0;
    while (waited < budget_ms && buf.size() < 65536) {
        int pr = ::poll(&pfd, 1, 200);
        if (pr < 0) break;
        if (pr == 0) { waited += 200; continue; }   // idle slice
        ssize_t r = ::read(fd, rd, sizeof(rd));
        if (r < 0) { if (errno == EAGAIN) { waited += 5; continue; } break; }
        if (r == 0) break;
        buf.insert(buf.end(), rd, rd + r);
        // Got data — once we hold a plausible full frame (a NAV-PVT is ~100 B),
        // give a brief grace for a trailing NAV-ATT then stop.
        if (buf.size() >= 100) {
            ::poll(&pfd, 1, 120);
            ssize_t r2 = ::read(fd, rd, sizeof(rd));
            if (r2 > 0) buf.insert(buf.end(), rd, rd + r2);
            break;
        }
    }
    ::close(fd);
    if (buf.empty()) { f.note = "rtk: no UBX data on " + dev; return f; }

    // Scan for the most recent valid NAV-PVT (0x01/0x07) AND NAV-ATT (0x01/0x05,
    // F9R fusion attitude) frames in this poll's buffer — one pass, take the LAST
    // valid of each. NAV-ATT is optional (F9R only, once fusion converges).
    const uint8_t* found = nullptr;   // NAV-PVT payload
    const uint8_t* att   = nullptr;   // NAV-ATT payload
    for (size_t i = 0; i + 8 <= buf.size(); ++i) {
        if (buf[i] != 0xB5 || buf[i + 1] != 0x62) continue;
        uint8_t cls = buf[i + 2], id = buf[i + 3];
        uint16_t len = rd_u16(&buf[i + 4]);
        size_t total = 6 + len + 2;               // sync..checksum
        if (i + total > buf.size()) continue;      // truncated tail
        if (cls != 0x01) continue;
        if (id == 0x07 && len >= 92) {             // NAV-PVT
            if (ubx_cksum_ok(&buf[i], 4 + len)) found = &buf[i + 6];
        } else if (id == 0x05 && len >= 32) {      // NAV-ATT
            if (ubx_cksum_ok(&buf[i], 4 + len)) att = &buf[i + 6];
        }
        // keep scanning — take the LAST valid frame of each this poll
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

    // Position accuracy (NAV-PVT off 40 U4 hAcc, 44 U4 vAcc — mm, 1-sigma).
    f.h_acc_m = static_cast<uint32_t>(rd_i32(found + 40)) * 1e-3;
    f.v_acc_m = static_cast<uint32_t>(rd_i32(found + 44)) * 1e-3;

    // Velocity + heading (off 48 I4 velN, 52 I4 velE, 56 I4 velD — mm/s;
    // 60 I4 gSpeed mm/s; 64 I4 headMot 1e-5 deg; 68 U4 sAcc mm/s;
    // 72 U4 headAcc 1e-5 deg). Present on any 2D/3D fix (carrSoln not required).
    if (fixType >= 2) {
        f.velocity_valid  = true;
        f.vel_n           = rd_i32(found + 48) * 1e-3;
        f.vel_e           = rd_i32(found + 52) * 1e-3;
        f.vel_d           = rd_i32(found + 56) * 1e-3;
        f.ground_speed    = rd_i32(found + 60) * 1e-3;
        f.heading_deg     = rd_i32(found + 64) * 1e-5;
        f.speed_acc_m_s   = static_cast<uint32_t>(rd_i32(found + 68)) * 1e-3;
        f.heading_acc_deg = static_cast<uint32_t>(rd_i32(found + 72)) * 1e-5;
    }

    // Vehicle attitude (UBX-NAV-ATT, F9R fusion — off 8 I4 roll, 12 I4 pitch,
    // 16 I4 heading — all 1e-5 deg; 20 U4 accRoll, 24 U4 accPitch, 28 U4
    // accHeading — 1e-5 deg). Only present once fusion converges (fusionMode>0,
    // needs motion); absent on F9P/stationary. accHeading==0 is the "not yet
    // valid" sentinel u-blox uses before the attitude solution settles.
    if (att) {
        double acc_head = static_cast<uint32_t>(rd_i32(att + 28)) * 1e-5;
        if (acc_head > 0.0) {
            f.attitude_valid       = true;
            f.roll_deg             = rd_i32(att + 8)  * 1e-5;
            f.pitch_deg            = rd_i32(att + 12) * 1e-5;
            f.att_heading_deg      = rd_i32(att + 16) * 1e-5;
            f.roll_acc_deg         = static_cast<uint32_t>(rd_i32(att + 20)) * 1e-5;
            f.pitch_acc_deg        = static_cast<uint32_t>(rd_i32(att + 24)) * 1e-5;
            f.heading_att_acc_deg  = acc_head;
        }
    }

    if (utc && fixType >= 2) {
        f.valid = true; f.utc_ns = utc;
        f.note = std::string("rtk fix type=") + std::to_string(fixType) +
                 (carrSoln == 2 ? " RTK-fixed" : carrSoln == 1 ? " RTK-float" : "") +
                 (f.attitude_valid ? " +ATT" : "");
    } else {
        f.note = "rtk: NAV-PVT no time/fix yet";
    }
    return f;
}

const char* GpsBackend::name() { return "rtk"; }

}  // namespace ara::tsync
