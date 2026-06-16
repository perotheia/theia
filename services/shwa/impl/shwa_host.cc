// shwa_host — the DEFAULT (non-Jetson) telemetry backend.
//
// "Makes the adaptation for a host without a device tree" (the ticket): reads
// the generic Linux sources so SHWA on the central x86 node reports REAL
// numbers — /proc/stat (CPU% via a jiffies delta), /proc/meminfo (memory),
// /sys/class/thermal (temp), nvidia-smi (a discrete GPU, if present), and
// /sys/class/hwmon fan. The power plane is UNAVAILABLE here (no nvpmodel).
//
// Selected by the BUILD when --define jetson=on is NOT set. Exactly one backend
// .cc is linked; this is the default.

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
    char buf[512]; size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

// nvidia-smi is the only stable interface a discrete NVIDIA GPU exposes for
// util/mem/temp/power — there is no /sys equivalent. A short popen, GPU-only.
std::string nvsmi_(const std::string& query) {
    std::string out;
    FILE* p = ::popen(("nvidia-smi --query-gpu=" + query +
                       " --format=csv,noheader,nounits 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[256];
    if (std::fgets(buf, sizeof(buf), p)) out = buf;
    ::pclose(p);
    return out;
}

// /proc/stat first line "cpu  u n s idle iowait …" → busy/total jiffies.
void cpu_jiffies_(uint64_t& busy, uint64_t& total) {
    busy = total = 0;
    std::string s = slurp_("/proc/stat");
    if (s.rfind("cpu ", 0) != 0 && s.rfind("cpu\t", 0) != 0) return;
    unsigned long long v[10] = {0};
    std::sscanf(s.c_str() + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    uint64_t idle = v[3] + v[4];   // idle + iowait
    for (int i = 0; i < 10; ++i) total += v[i];
    busy = total - idle;
}

// Persisted CPU baseline (this backend is sampled once per tick from one thread).
uint64_t g_last_busy = 0, g_last_total = 0;

uint32_t fan_rpm_() {
    // First hwmon with a fan1_input.
    DIR* d = ::opendir("/sys/class/hwmon");
    if (!d) return 0;
    uint32_t rpm = 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string f = std::string("/sys/class/hwmon/") + e->d_name + "/fan1_input";
        std::string v = slurp_(f);
        if (!v.empty()) { rpm = (uint32_t)std::strtoul(v.c_str(), nullptr, 10); break; }
    }
    ::closedir(d);
    return rpm;
}

uint32_t hottest_temp_c_() {
    DIR* d = ::opendir("/sys/class/thermal");
    if (!d) return 0;
    long max_mC = 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        if (std::strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string v = slurp_(std::string("/sys/class/thermal/") + e->d_name + "/temp");
        if (v.empty()) continue;
        long mC = std::strtol(v.c_str(), nullptr, 10);
        if (mC > max_mC) max_mC = mC;
    }
    ::closedir(d);
    return (uint32_t)(max_mC / 1000);
}

}  // namespace

namespace backend {

void init() { cpu_jiffies_(g_last_busy, g_last_total); }

bool on_jetson() { return false; }
int  read_power_mode() { return PM_UNKNOWN; }
bool apply_power_mode(int /*mode*/, bool /*jc*/, std::string& err) {
    err = "no nvpmodel (host backend — not a Jetson)";
    return false;
}

void sample(AccelReading& r) {
    r.board = "x86_64";
    r.on_jetson = false;
    r.power_mode = PM_UNKNOWN;

    // CPU%: delta vs the last sample.
    uint64_t busy, total;
    cpu_jiffies_(busy, total);
    if (total > g_last_total) {
        uint64_t db = busy - g_last_busy, dt = total - g_last_total;
        r.cpu_util_pct = dt ? (uint32_t)(db * 100 / dt) : 0;
    }
    g_last_busy = busy; g_last_total = total;
    r.cpu_count = (uint32_t)::sysconf(_SC_NPROCESSORS_ONLN);

    // CPU freq (avg of cpu0 cur_freq, kHz → MHz; best-effort).
    {
        std::string khz = slurp_("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
        if (!khz.empty()) r.cpu_freq_mhz = (uint32_t)(std::strtoul(khz.c_str(), nullptr, 10) / 1000);
    }

    // Memory (whole-system, /proc/meminfo).
    {
        std::string mi = slurp_("/proc/meminfo");
        auto kb = [&](const char* key) -> uint64_t {
            auto p = mi.find(key);
            if (p == std::string::npos) return 0;
            return std::strtoull(mi.c_str() + p + std::strlen(key), nullptr, 10);
        };
        uint64_t tot = kb("MemTotal:"), avail = kb("MemAvailable:");
        r.mem_total_mb = (uint32_t)(tot / 1024);
        r.mem_used_mb  = (uint32_t)((tot > avail ? tot - avail : 0) / 1024);
    }

    // GPU (discrete NVIDIA via nvidia-smi, if present).
    {
        std::string u = nvsmi_("utilization.gpu");
        if (!u.empty()) {
            r.gpu_util_pct = (uint32_t)std::strtoul(u.c_str(), nullptr, 10);
            std::string f = nvsmi_("clocks.sm");
            if (!f.empty()) r.gpu_freq_mhz = (uint32_t)std::strtoul(f.c_str(), nullptr, 10);
            std::string p = nvsmi_("power.draw");
            if (!p.empty()) r.power_mw = (uint32_t)(std::strtod(p.c_str(), nullptr) * 1000);
        }
    }

    r.temp_c  = hottest_temp_c_();
    r.fan_rpm = fan_rpm_();
}

}  // namespace backend
}  // namespace ara::shwa
