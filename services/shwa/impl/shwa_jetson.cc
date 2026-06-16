// shwa_jetson — the NVIDIA Jetson (Orin) telemetry + power backend.
//
// Reads the Orin SoC sysfs the way jetson-stats (jtop) does — GPU via the
// devfreq domain (/sys/class/devfreq/*gpu*), CPU via /sys/devices/system/cpu,
// memory via /proc/meminfo, thermals via the SoC thermal zones — and OWNS the
// power plane (nvpmodel profile + jetson_clocks). The image of the relevant
// jtop paths is in up/jetson_stats/jtop/core/.
//
// Selected by the BUILD only when --define jetson=on. NOT built on the dev host
// (no device tree); the host backend (shwa_host.cc) is the default. Exactly one
// backend .cc is linked.

#include "impl/shwa_backend.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unistd.h>

namespace ara::shwa {
namespace {

std::string slurp_(const std::string& p) {
    std::string out;
    FILE* f = ::fopen(p.c_str(), "r");
    if (!f) return out;
    char buf[256]; size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

std::string run_(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    return out;
}

// The Orin GPU devfreq domain (jtop: hw_detect.py — gpu-gpc-0 / 17000000.gpu).
std::string gpu_devfreq_() {
    for (const char* d : {"/sys/class/devfreq/17000000.gpu",
                          "/sys/class/devfreq/gpu-gpc-0",
                          "/sys/class/devfreq/57000000.gpu"})
        if (!slurp_(std::string(d) + "/cur_freq").empty()) return d;
    return "";
}

uint64_t g_last_busy = 0, g_last_total = 0;

void cpu_jiffies_(uint64_t& busy, uint64_t& total) {
    busy = total = 0;
    std::string s = slurp_("/proc/stat");
    if (s.rfind("cpu ", 0) != 0) return;
    unsigned long long v[10] = {0};
    std::sscanf(s.c_str() + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    uint64_t idle = v[3] + v[4];
    for (int i = 0; i < 10; ++i) total += v[i];
    busy = total - idle;
}

uint32_t hottest_temp_c_() {
    DIR* d = ::opendir("/sys/class/thermal");
    if (!d) return 0;
    long max_mC = 0; struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string v = slurp_(std::string("/sys/class/thermal/") + e->d_name + "/temp");
        if (!v.empty()) { long mC = std::strtol(v.c_str(), nullptr, 10);
            if (mC > max_mC) max_mC = mC; }
    }
    ::closedir(d);
    return (uint32_t)(max_mC / 1000);
}

}  // namespace

namespace backend {

void init() { cpu_jiffies_(g_last_busy, g_last_total); }

bool on_jetson() { return true; }

// nvpmodel -q → active profile id → PowerMode (jtop's nvpmodel mapping).
int read_power_mode() {
    std::string q = run_("nvpmodel -q");
    if (q.empty()) return PM_UNKNOWN;
    int id = -1;
    for (size_t i = 0; i < q.size(); ++i)
        if (std::isdigit((unsigned char)q[i])) {
            id = std::atoi(q.c_str() + i);
            while (i < q.size() && std::isdigit((unsigned char)q[i])) ++i;
        }
    switch (id) { case 0: return PM_MAXN; case 1: return PM_BALANCED;
                  default: return id < 0 ? PM_UNKNOWN : PM_LOW; }
}

bool apply_power_mode(int mode, bool jetson_clocks, std::string& err) {
    int id;
    switch (mode) {
        case PM_MAXN: id = 0; break; case PM_BALANCED: id = 1; break;
        case PM_LOW: id = 2; break;
        default: err = "MODE_UNKNOWN — leaving platform default"; return false;
    }
    run_("nvpmodel -m " + std::to_string(id));
    if (jetson_clocks) run_("jetson_clocks");
    return true;
}

void sample(AccelReading& r) {
    r.board = "jetson";
    r.on_jetson = true;
    r.power_mode = read_power_mode();

    uint64_t busy, total; cpu_jiffies_(busy, total);
    if (total > g_last_total) {
        uint64_t db = busy - g_last_busy, dt = total - g_last_total;
        r.cpu_util_pct = dt ? (uint32_t)(db * 100 / dt) : 0;
    }
    g_last_busy = busy; g_last_total = total;
    r.cpu_count = (uint32_t)::sysconf(_SC_NPROCESSORS_ONLN);
    {
        std::string khz = slurp_("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
        if (!khz.empty()) r.cpu_freq_mhz = (uint32_t)(std::strtoul(khz.c_str(), nullptr, 10) / 1000);
    }

    // GPU via the devfreq domain (load = cur_freq/max_freq is approximate; the
    // load node `device/load` is per-SoC, fall back to a freq ratio).
    std::string gp = gpu_devfreq_();
    if (!gp.empty()) {
        std::string cur = slurp_(gp + "/cur_freq"), mx = slurp_(gp + "/max_freq");
        std::string load = slurp_(gp + "/device/load");
        unsigned long c = std::strtoul(cur.c_str(), nullptr, 10);
        r.gpu_freq_mhz = (uint32_t)(c / 1000000);
        if (!load.empty()) r.gpu_util_pct = (uint32_t)std::strtoul(load.c_str(), nullptr, 10);
        else { unsigned long m = std::strtoul(mx.c_str(), nullptr, 10);
               if (m) r.gpu_util_pct = (uint32_t)(c * 100 / m); }
    }

    {
        std::string mi = slurp_("/proc/meminfo");
        auto kb = [&](const char* k) -> uint64_t { auto p = mi.find(k);
            return p == std::string::npos ? 0 : std::strtoull(mi.c_str()+p+std::strlen(k), nullptr, 10); };
        uint64_t tot = kb("MemTotal:"), avail = kb("MemAvailable:");
        r.mem_total_mb = (uint32_t)(tot / 1024);
        r.mem_used_mb  = (uint32_t)((tot > avail ? tot - avail : 0) / 1024);
    }

    r.temp_c = hottest_temp_c_();
    // Board power: the Orin INA3221 rail (jtop reads /sys/bus/i2c .../in_power0_input).
    std::string pw = slurp_("/sys/bus/i2c/drivers/ina3221/1-0040/hwmon/hwmon0/in_power0_input");
    if (!pw.empty()) r.power_mw = (uint32_t)std::strtoul(pw.c_str(), nullptr, 10);
}

}  // namespace backend
}  // namespace ara::shwa
