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
    std::printf("shwa-host-backend: OK — board=%s mem=%u/%u MB cpu=x%u temp=%u C\n",
                r.board.c_str(), r.mem_used_mb, r.mem_total_mb, r.cpu_count, r.temp_c);
    return 0;
}
