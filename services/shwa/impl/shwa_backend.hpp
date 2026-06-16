// shwa_backend — the hardware-telemetry source behind ShwaDaemon.
//
// APP-OWNED. The INTERFACE only: one `sample()` that fills an AccelReading from
// the host. Two implementations swap at BUILD time via a bazel select (module
// inclusion, NOT #ifdef): shwa_host.cc (default — /proc/stat, /proc/meminfo,
// /sys/class/thermal, nvidia-smi, hwmon fan, so SHWA on the central x86 host
// reports REAL numbers) and shwa_jetson.cc (--define jetson=on — the Orin /sys
// SoC nodes jtop reads). Exactly one is compiled in.
//
// Power-mode control (nvpmodel/jetson_clocks) is also backend-specific: the
// jetson backend manages it, the host backend reports MODE_UNKNOWN + a no-op
// apply. This is the plane relocated here from osi.

#pragma once

#include <cstdint>
#include <string>

namespace ara::shwa {

// PowerMode ordinals (.art): 0=UNKNOWN 1=MAXN 2=BALANCED 3=LOW.
enum PMode : int { PM_UNKNOWN = 0, PM_MAXN = 1, PM_BALANCED = 2, PM_LOW = 3 };

// One telemetry reading — mirrors the .art AccelSample (filled by the backend).
struct AccelReading {
    std::string board;
    bool        on_jetson = false;
    int         power_mode = PM_UNKNOWN;

    uint32_t    cpu_util_pct = 0;
    uint32_t    cpu_count = 0;
    uint32_t    cpu_freq_mhz = 0;

    uint32_t    gpu_util_pct = 0;
    uint32_t    gpu_freq_mhz = 0;

    uint32_t    mem_used_mb = 0;
    uint32_t    mem_total_mb = 0;

    uint32_t    temp_c = 0;
    uint32_t    power_mw = 0;
    uint32_t    fan_rpm = 0;
};

// The backend interface — ONE implementation is linked (host or jetson).
//   init()                — one-time setup (open sysfs handles / cache board id).
//   sample(out)           — fill `out` with the current reading.
//   on_jetson()           — is this the Jetson backend?
//   read_power_mode()     — the active nvpmodel profile (PM_UNKNOWN off-Jetson).
//   apply_power_mode(...) — set it on Orin; no-op + false off-Jetson.
namespace backend {

void init();
void sample(AccelReading& out);
bool on_jetson();
int  read_power_mode();
bool apply_power_mode(int mode, bool jetson_clocks, std::string& err);

}  // namespace backend
}  // namespace ara::shwa
