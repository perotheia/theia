// rds_latency_bench — the RDS path's own latency, from a writer's Loan to a reader's
// Take, for N streams written at the same instant (a camera rig: one writer thread
// per sensor, like carla_ws SimTick's Listen callbacks) and read by ONE reader in
// another process that polls all N (like spr's SprTruth: Take until empty on each,
// then wait poll_ms).
//
// Needs the host RouDi (iox-roudi) running. Prints one JSON line of percentiles (us):
//   loan     Loan() call
//   fill     writing the payload (memcpy of `bytes`: the adapter's encode copy)
//   publish  Publish() call
//   deliver  Publish() -> the reader's Take() (iceoryx + the reader's poll wait)
//   w_spread first -> last Publish of one frame (writer side)
//   r_spread first -> last Take of one frame (reader side): what spr calls the
//            "spread of the 8 chunks"
//   total    frame start -> last Take
//
//   rds_latency_bench [--writers 8] [--bytes 1048576] [--frames 200] [--period-ms 100]
//                     [--poll-ms 2] (0: spin) [--wake 1] (OnData, no poll)
//                     [--fill memcpy|none] [--prefix Bench]
#include "ara/rds/stream.h"
#include "ara/rds/raw_data_stream.h"
#include "ara/rds/transport_if.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace {

uint64_t mono_ns() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
void sleep_until(uint64_t t) {
    for (;;) {
        const uint64_t now = mono_ns();
        if (now >= t) return;
        timespec d{ time_t((t - now) / 1000000000ull), long((t - now) % 1000000000ull) };
        nanosleep(&d, nullptr);
    }
}

struct Stamps {          // written right after the FrameDescriptor
    uint64_t start, loaned, filled, published;
    uint32_t writer, frame;
};

struct Args {
    int writers = 8, frames = 200, period_ms = 100, poll_ms = 2;
    bool wake = false;   // --wake 1: StreamReader::OnData wakes the reader (no poll)
    uint32_t bytes = 1u << 20;
    bool fill = true;
    std::string prefix = "Bench";
};

std::string stream_of(const Args& a, int i) { return "/" + a.prefix + "/s" + std::to_string(i); }

double pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
}

int run_writers(const Args& a, uint64_t t0) {
    ara::rds::Runtime::Init("rds_bench_w");
    std::vector<std::unique_ptr<ara::rds::StreamWriter>> w;
    for (int i = 0; i < a.writers; ++i) {
        w.push_back(std::make_unique<ara::rds::StreamWriter>(
            ara::rds::resolve_instance(stream_of(a, i), a.bytes + 4096, 0)));
        if (w.back()->Connect() != ara::rds::RdsError::OK) { std::fprintf(stderr, "writer connect failed\n"); return 2; }
    }
    std::vector<uint8_t> src(a.bytes, 0x5a);
    std::vector<std::thread> th;
    std::atomic<int> drops{0};
    for (int i = 0; i < a.writers; ++i)
        th.emplace_back([&, i] {
            for (int f = 0; f < a.frames; ++f) {
                const uint64_t start = t0 + uint64_t(f) * a.period_ms * 1000000ull;
                sleep_until(start);
                const uint64_t s0 = mono_ns();
                auto c = w[i]->Loan(uint32_t(sizeof(ara::rds::FrameDescriptor) + sizeof(Stamps) + a.bytes));
                if (!c.ok()) { ++drops; continue; }
                const uint64_t s1 = mono_ns();
                auto* d = static_cast<ara::rds::FrameDescriptor*>(c.value.data);
                auto* st = reinterpret_cast<Stamps*>(d + 1);
                if (a.fill) std::memcpy(reinterpret_cast<uint8_t*>(st + 1), src.data(), a.bytes);
                const uint64_t s2 = mono_ns();
                d->frame_id = f; d->payload_size = a.bytes;
                *st = Stamps{start, s1 - s0, s2 - s1, 0, uint32_t(i), uint32_t(f)};
                st->published = mono_ns();
                w[i]->Publish(std::move(c.value));
                st = nullptr;   // the chunk is the pool's now
            }
        });
    for (auto& t : th) t.join();
    if (drops) std::fprintf(stderr, "writers: %d loans failed (pool too small?)\n", drops.load());
    sleep_until(mono_ns() + 300000000ull);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--writers") a.writers = std::atoi(v.c_str());
        else if (k == "--bytes") a.bytes = uint32_t(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "--frames") a.frames = std::atoi(v.c_str());
        else if (k == "--period-ms") a.period_ms = std::atoi(v.c_str());
        else if (k == "--poll-ms") a.poll_ms = std::atoi(v.c_str());
        else if (k == "--fill") a.fill = v != "none";
        else if (k == "--prefix") a.prefix = v;
        else if (k == "--wake") a.wake = v != "0";
        else { std::fprintf(stderr, "unknown %s\n", k.c_str()); return 2; }
    }
    const uint64_t t0 = mono_ns() + 1500000000ull;   // the reader subscribes first
    const pid_t pid = fork();
    if (pid == 0) return run_writers(a, t0);

    ara::rds::Runtime::Init("rds_bench_r");
    std::vector<std::unique_ptr<ara::rds::StreamReader>> r;
    for (int i = 0; i < a.writers; ++i) {
        r.push_back(std::make_unique<ara::rds::StreamReader>(
            ara::rds::resolve_instance(stream_of(a, i), a.bytes + 4096, 0)));
        if (r.back()->Connect() != ara::rds::RdsError::OK) { std::fprintf(stderr, "reader connect failed\n"); return 2; }
    }
    // --wake: like a node that posts itself an info from OnData and drains there
    std::mutex wmu; std::condition_variable wcv; bool pending = false;
    if (a.wake)
        for (auto& rd : r)
            if (rd->OnData([&] { { std::lock_guard<std::mutex> l(wmu); pending = true; } wcv.notify_one(); })
                    != ara::rds::RdsError::OK) { std::fprintf(stderr, "OnData failed\n"); return 2; }
    std::vector<double> loan, fill, deliver, w_spread, r_spread, total;
    struct F { uint64_t start = 0, pub_lo = ~0ull, pub_hi = 0, take_lo = ~0ull, take_hi = 0; int n = 0; };
    std::map<uint32_t, F> fr;
    const uint64_t end = t0 + uint64_t(a.frames) * a.period_ms * 1000000ull + 500000000ull;
    long got = 0;
    while (mono_ns() < end) {
        for (int i = 0; i < a.writers; ++i)
            for (;;) {
                auto c = r[i]->Take();
                if (!c.ok()) break;
                const uint64_t now = mono_ns();
                const auto* st = reinterpret_cast<const Stamps*>(
                    static_cast<const ara::rds::FrameDescriptor*>(c.value.data) + 1);
                loan.push_back(st->loaned / 1e3); fill.push_back(st->filled / 1e3);
                deliver.push_back((now - st->published) / 1e3);
                F& f = fr[st->frame];
                f.start = st->start; ++f.n;
                f.pub_lo = std::min(f.pub_lo, st->published); f.pub_hi = std::max(f.pub_hi, st->published);
                f.take_lo = std::min(f.take_lo, now); f.take_hi = std::max(f.take_hi, now);
                r[i]->Release(std::move(c.value));
                ++got;
            }
        if (a.wake) {
            std::unique_lock<std::mutex> l(wmu);
            wcv.wait_for(l, std::chrono::milliseconds(100), [&] { return pending; });
            pending = false;
        } else if (a.poll_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(a.poll_ms));
    }
    int complete = 0;
    for (auto& [k, f] : fr)
        if (f.n == a.writers) {
            ++complete;
            w_spread.push_back((f.pub_hi - f.pub_lo) / 1e3);
            r_spread.push_back((f.take_hi - f.take_lo) / 1e3);
            total.push_back((f.take_hi - f.start) / 1e3);
        }
    int status = 0; waitpid(pid, &status, 0);
    auto P = [](const char* n, const std::vector<double>& v) {
        std::printf("\"%s\": {\"p50\": %.1f, \"p99\": %.1f, \"max\": %.1f}", n, pct(v, .5), pct(v, .99), pct(v, 1));
    };
    std::printf("{\"wake\": %s, \"writers\": %d, \"bytes\": %u, \"fill_mode\": %s, \"poll_ms\": %d, \"frames\": %d, \"chunks\": %ld, \"complete\": %d, ",
                a.wake ? "true" : "false", a.writers, a.bytes, a.fill ? "\"memcpy\"" : "\"none\"", a.poll_ms, a.frames, got, complete);
    P("loan", loan); std::printf(", "); P("fill", fill); std::printf(", "); P("deliver", deliver); std::printf(", ");
    P("w_spread", w_spread); std::printf(", "); P("r_spread", r_spread); std::printf(", "); P("total", total);
    std::printf(", \"unit\": \"us\"}\n");
    return 0;
}
