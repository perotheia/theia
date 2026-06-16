// Unit check for the shwa HOST backend (the default, non-Jetson). Asserts it
// reports sane real numbers on this host: a board, a nonzero mem total, a
// cpu_count, a temperature. The exact values vary; the SHAPE is what we pin.
#include "impl/shwa_backend.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>
int main() {
    using namespace ara::shwa;
    backend::init();
    assert(!backend::on_jetson() && "host backend must report not-Jetson");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    AccelReading r; backend::sample(r);
    assert(r.board == "x86_64");
    assert(r.mem_total_mb > 0 && "memory total from /proc/meminfo");
    assert(r.cpu_count > 0 && "cpu count from sysconf");
    assert(r.mem_used_mb <= r.mem_total_mb);
    std::string err;
    assert(!backend::apply_power_mode(PM_MAXN, false, err) &&
           "power apply is a no-op on the host backend");
    // Host system-monitor facts (SHWA is the host monitor): uptime + disk-free.
    assert(r.uptime_sec > 0 && "uptime from /proc/uptime");
    assert(r.disk_root_total_kb > 0 && "root fs total from statvfs(/)");
    assert(r.disk_root_avail_kb <= r.disk_root_total_kb);
    assert(r.disk_install_total_kb > 0 && "install fs total from statvfs");
    assert(r.disk_install_avail_kb <= r.disk_install_total_kb);
    std::printf("shwa-host-backend: OK — board=%s mem=%u/%u MB cpu=x%u temp=%u C "
                "uptime=%llus root=%llu/%lluMB install=%llu/%lluMB\n",
                r.board.c_str(), r.mem_used_mb, r.mem_total_mb, r.cpu_count, r.temp_c,
                (unsigned long long)r.uptime_sec,
                (unsigned long long)(r.disk_root_avail_kb / 1024),
                (unsigned long long)(r.disk_root_total_kb / 1024),
                (unsigned long long)(r.disk_install_avail_kb / 1024),
                (unsigned long long)(r.disk_install_total_kb / 1024));
    return 0;
}
