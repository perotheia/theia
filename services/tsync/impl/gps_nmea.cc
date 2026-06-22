// gps_nmea — the NMEA GNSS variant (`--define gps=nmea`). Reads a serial tty and
// parses the two time/position sentences any receiver emits:
//   $GxRMC — UTC time + date + lat/lon + status (A=valid, V=void)
//   $GxGGA — UTC time + lat/lon/alt + fix quality
// (Gx = GP/GN/GL/GA depending on constellation.) This is the lowest-common-
// denominator path — a cheap receiver with no UBX. The dead-reckoning / RTK
// detail NMEA can't carry is why gps_rtk.cc exists; here rtk_fix is always false.
//
// Open-read-parse-close per poll (v1): at a ~1Hz tick we read whatever lines are
// buffered, take the most recent complete RMC (it carries the date, which GGA
// lacks), and build a GnssFix. No termios reconfigure — the deploy sets the tty
// baud (stty) at stage time; we just read.

#include "impl/gps_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace ara::tsync {

namespace {

constexpr const char* kDefaultDev = "/dev/serial0";

// Split a comma-separated NMEA body into fields (no checksum validation in v1 —
// a malformed line just yields a !valid fix and we wait for the next poll).
int split_fields(char* line, char* out[], int max) {
    int n = 0;
    char* p = line;
    out[n++] = p;
    while (*p && n < max) {
        if (*p == ',') { *p = '\0'; out[n++] = p + 1; }
        ++p;
    }
    return n;
}

// "ddmm.mmmm" + hemisphere → signed decimal degrees.
double nmea_coord(const char* v, const char* hemi) {
    if (!v || !*v) return 0.0;
    double raw = ::strtod(v, nullptr);
    double deg = static_cast<int>(raw / 100);
    double min = raw - deg * 100.0;
    double d = deg + min / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

// RMC time "hhmmss.sss" + date "ddmmyy" → epoch nanoseconds (UTC).
uint64_t rmc_utc_ns(const char* t, const char* date) {
    if (!t || !*t || !date || !*date || std::strlen(date) < 6) return 0;
    struct tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    int hh = (t[0]-'0')*10 + (t[1]-'0');
    int mm = (t[2]-'0')*10 + (t[3]-'0');
    int ss = (t[4]-'0')*10 + (t[5]-'0');
    int day = (date[0]-'0')*10 + (date[1]-'0');
    int mon = (date[2]-'0')*10 + (date[3]-'0');
    int yr  = (date[4]-'0')*10 + (date[5]-'0');
    tmv.tm_hour = hh; tmv.tm_min = mm; tmv.tm_sec = ss;
    tmv.tm_mday = day; tmv.tm_mon = mon - 1; tmv.tm_year = 100 + yr;  // 20xx
    time_t secs = ::timegm(&tmv);
    if (secs == (time_t)-1) return 0;
    // fractional seconds after the '.', if present
    long frac_ns = 0;
    const char* dot = std::strchr(t, '.');
    if (dot) {
        double f = ::strtod(dot, nullptr);  // 0.sss
        frac_ns = static_cast<long>(f * 1e9);
    }
    return static_cast<uint64_t>(secs) * 1000000000ULL + frac_ns;
}

}  // namespace

GnssFix GpsBackend::poll(const std::string& dev_in, uint32_t /*baud*/) {
    GnssFix f;
    const std::string dev = dev_in.empty() ? kDefaultDev : dev_in;

    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) { f.note = "nmea: open(" + dev + ") failed"; return f; }

    // Drain what's buffered (cap so a chatty tty can't stall the poll).
    std::string buf;
    char rd[1024];
    for (int i = 0; i < 16; ++i) {
        ssize_t r = ::read(fd, rd, sizeof(rd));
        if (r <= 0) break;
        buf.append(rd, static_cast<size_t>(r));
        if (buf.size() > 16384) break;
    }
    ::close(fd);
    if (buf.empty()) { f.note = "nmea: no data on " + dev; return f; }

    // Walk lines; keep the most recent valid RMC (carries date+time) and the
    // most recent GGA (carries altitude).
    uint64_t utc = 0; double lat = 0, lon = 0, alt = 0; bool have_rmc = false;
    size_t pos = 0;
    while (pos < buf.size()) {
        size_t nl = buf.find('\n', pos);
        std::string line = buf.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? buf.size() : nl + 1;
        if (line.size() < 6 || line[0] != '$') continue;
        char* fields[24];
        std::string mut = line;
        int n = split_fields(&mut[0], fields, 24);
        const char* tag = fields[0] + 3;   // skip "$Gx"
        if (std::strncmp(tag, "RMC", 3) == 0 && n >= 10) {
            // $GxRMC,time,status,lat,N,lon,E,spd,cog,date,...
            if (fields[2][0] == 'A') {       // A=valid
                utc = rmc_utc_ns(fields[1], fields[9]);
                lat = nmea_coord(fields[3], fields[4]);
                lon = nmea_coord(fields[5], fields[6]);
                if (utc) have_rmc = true;
            }
        } else if (std::strncmp(tag, "GGA", 3) == 0 && n >= 10) {
            // $GxGGA,time,lat,N,lon,E,quality,nsat,hdop,alt,M,...
            if (fields[6][0] != '0')         // quality 0 = no fix
                alt = ::strtod(fields[9], nullptr);
        }
    }

    if (have_rmc) {
        f.valid = true; f.utc_ns = utc; f.lat = lat; f.lon = lon; f.alt = alt;
        f.note = "nmea fix";
    } else {
        f.note = "nmea: no valid RMC on " + dev;
    }
    return f;
}

const char* GpsBackend::name() { return "nmea"; }

}  // namespace ara::tsync
