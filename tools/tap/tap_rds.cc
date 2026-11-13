// tap_rds — record / replay ANY ara::rds stream as raw chunks (INTEGRATION.md 4).
//
// The RDS half of the tap (tools/tap/tap.py drives it; it runs standalone too).
// No node, no supervisor: one iceoryx runtime per process, attached to the host
// RouDi. It never interprets a chunk: what was on the stream is what is written
// and what is played back (a FrameDescriptor + pixels for a camera, anything for
// any other stream).
//
// The recording directory (the shape of spr's rig recorder, spr_rig.hh, so either
// tool plays the other's recordings):
//   streams.txt   one line per stream: "<file> <stream instance>", e.g. "Ego_dep_fl.bin /Ego/dep_fl"
//   <file>.bin    records: {uint64 rx_wall_ns, uint32 bytes, uint32 0} + the chunk's `bytes`
//
//   tap_rds record <dir> <stream>[,<stream>...] [--seconds S] [--chunk BYTES]
//   tap_rds replay <dir> [--speed X] [--start-wall-ns T --rx0-ns R] [--start-delay-ms D]
//                        [--only <stream>[,...]] [--loop]
//
// record: until SIGINT/SIGTERM or S seconds; prints one JSON summary line.
// replay: every chunk is published on its RECORDED stream name at
//   start + (rx_wall_ns - rx0) / speed (CLOCK_REALTIME). By default start = now +
//   start_delay (subscribers connect first) and rx0 = the first record of the
//   recording; tap.py passes both so the TIPC and RDS halves share one clock.
//   Chunks go out verbatim: stamps inside them are the recorded ones.
//   No live writer may publish the same stream at the same time.

#include "ara/rds/stream.h"
#include "ara/rds/transport_if.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct RecordHdr {
    uint64_t rx_wall_ns;
    uint32_t bytes;
    uint32_t reserved;
};
static_assert(sizeof(RecordHdr) == 16, "record header is 16 bytes (spr_rig.hh RigRecord)");

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

uint64_t wall_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void sleep_until_wall(uint64_t t) {
    for (;;) {
        const uint64_t now = wall_ns();
        if (now >= t || g_stop) return;
        const uint64_t d = t - now;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(d, 50000000ull)));
    }
}

// "/Ego/dep_fl" -> "Ego_dep_fl.bin" (spr_rig.hh file_of)
std::string file_of(const std::string& stream) {
    std::string s = stream;
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    for (char& c : s)
        if (c == '/') c = '_';
    return s + ".bin";
}

std::vector<std::string> split_csv(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string p;
    while (std::getline(ss, p, ','))
        if (!p.empty()) out.push_back(p);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

int usage() {
    std::fprintf(stderr,
                 "usage: tap_rds record <dir> <stream>[,<stream>...] [--seconds S] [--chunk BYTES]\n"
                 "       tap_rds replay <dir> [--speed X] [--start-wall-ns T --rx0-ns R]\n"
                 "                      [--start-delay-ms D] [--only <stream>[,...]] [--loop]\n");
    return 2;
}

// ---- record ------------------------------------------------------------------
int cmd_record(const std::string& dir, const std::vector<std::string>& streams, double seconds,
               uint32_t chunk) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ::ara::rds::Runtime::Init("tap_rec_" + std::to_string(::getpid()));

    struct S {
        std::string path;
        std::unique_ptr<::ara::rds::StreamReader> rd;
        FILE* f = nullptr;
        uint64_t chunks = 0, bytes = 0;
    };
    std::vector<S> ss;
    // Append to streams.txt (a recording may be extended), one line per new stream.
    std::vector<std::string> known;
    {
        std::ifstream in(dir + "/streams.txt");
        std::string file, path;
        while (in >> file >> path) known.push_back(path);
    }
    FILE* idx = std::fopen((dir + "/streams.txt").c_str(), "a");
    for (const auto& p : streams) {
        S s;
        s.path = p;
        s.rd = std::make_unique<::ara::rds::StreamReader>(::ara::rds::resolve_instance(p, chunk, 0u));   // no history: only what arrives while recording
        if (s.rd->Connect() != ::ara::rds::RdsError::OK) {
            std::fprintf(stderr, "tap_rds: reader Connect failed on %s (RouDi up?)\n", p.c_str());
            return 1;
        }
        s.f = std::fopen((dir + "/" + file_of(p)).c_str(), "ab");
        if (!s.f) {
            std::fprintf(stderr, "tap_rds: cannot write %s\n", (dir + "/" + file_of(p)).c_str());
            return 1;
        }
        if (idx && std::find(known.begin(), known.end(), p) == known.end())
            std::fprintf(idx, "%s %s\n", file_of(p).c_str(), p.c_str());
        ss.push_back(std::move(s));
    }
    if (idx) std::fclose(idx);
    std::fprintf(stderr, "tap_rds: recording %zu stream(s) -> %s\n", ss.size(), dir.c_str());

    const uint64_t end = seconds > 0 ? wall_ns() + uint64_t(seconds * 1e9) : 0;
    uint64_t errors = 0;
    while (!g_stop && (end == 0 || wall_ns() < end)) {
        bool any = false;
        for (auto& s : ss) {
            for (;;) {
                auto r = s.rd->Take();
                if (!r.ok()) break;
                any = true;
                const RecordHdr h{wall_ns(), r.value.size, 0};
                const bool ok = std::fwrite(&h, sizeof h, 1, s.f) == 1 &&
                                std::fwrite(r.value.data, 1, r.value.size, s.f) == r.value.size;
                s.rd->Release(std::move(r.value));
                if (!ok) { ++errors; continue; }
                ++s.chunks;
                s.bytes += h.bytes;
            }
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("{\"mode\": \"record\", \"dir\": \"%s\", \"errors\": %llu, \"streams\": {", json_escape(dir).c_str(),
                (unsigned long long)errors);
    for (size_t i = 0; i < ss.size(); ++i) {
        std::fflush(ss[i].f);
        std::fclose(ss[i].f);
        ss[i].rd->Shutdown();
        std::printf("%s\"%s\": {\"chunks\": %llu, \"bytes\": %llu}", i ? ", " : "",
                    json_escape(ss[i].path).c_str(), (unsigned long long)ss[i].chunks,
                    (unsigned long long)ss[i].bytes);
    }
    std::printf("}}\n");
    return errors ? 1 : 0;
}

// ---- replay ------------------------------------------------------------------
struct Entry {
    uint64_t rx;
    uint32_t bytes;
    long offset;
    size_t stream;
};

int cmd_replay(const std::string& dir, double speed, uint64_t start_wall, uint64_t rx0_arg,
               uint32_t start_delay_ms, const std::vector<std::string>& only, bool loop) {
    std::ifstream in(dir + "/streams.txt");
    if (!in) {
        std::fprintf(stderr, "tap_rds: no %s/streams.txt\n", dir.c_str());
        return 1;
    }
    struct S {
        std::string path, file;
        FILE* f = nullptr;
        uint32_t max_bytes = 0;
        std::unique_ptr<::ara::rds::StreamWriter> wr;
        uint64_t chunks = 0;
    };
    std::vector<S> ss;
    std::vector<Entry> all;
    std::string file, path;
    while (in >> file >> path) {
        if (!only.empty() && std::find(only.begin(), only.end(), path) == only.end()) continue;
        S s;
        s.path = path;
        s.file = dir + "/" + file;
        s.f = std::fopen(s.file.c_str(), "rb");
        if (!s.f) {
            std::fprintf(stderr, "tap_rds: cannot read %s\n", s.file.c_str());
            return 1;
        }
        for (;;) {   // index the file
            RecordHdr h{};
            if (std::fread(&h, sizeof h, 1, s.f) != 1) break;
            const long off = std::ftell(s.f);
            if (std::fseek(s.f, h.bytes, SEEK_CUR) != 0) break;
            if (std::ftell(s.f) != off + long(h.bytes)) break;   // truncated tail: drop it
            all.push_back({h.rx_wall_ns, h.bytes, off, ss.size()});
            s.max_bytes = std::max(s.max_bytes, h.bytes);
        }
        ss.push_back(std::move(s));
    }
    if (all.empty()) {
        std::fprintf(stderr, "tap_rds: nothing to replay in %s\n", dir.c_str());
        return 1;
    }
    std::stable_sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.rx < b.rx; });

    ::ara::rds::Runtime::Init("tap_play_" + std::to_string(::getpid()));
    for (auto& s : ss) {
        s.wr = std::make_unique<::ara::rds::StreamWriter>(
            ::ara::rds::resolve_instance(s.path, s.max_bytes, 4u));
        if (s.wr->Connect() != ::ara::rds::RdsError::OK) {
            std::fprintf(stderr, "tap_rds: writer Connect failed on %s (RouDi up?)\n", s.path.c_str());
            return 1;
        }
    }
    const uint64_t rx0 = rx0_arg ? rx0_arg : all.front().rx;
    uint64_t start = start_wall ? start_wall : wall_ns() + uint64_t(start_delay_ms) * 1000000ull;
    const uint64_t span = all.back().rx - rx0;
    std::fprintf(stderr, "tap_rds: replaying %zu chunk(s) on %zu stream(s), %.3f s at x%.2f\n", all.size(),
                 ss.size(), double(span) / 1e9, speed);
    uint64_t late_max_ns = 0, errors = 0;
    std::vector<char> buf;
    do {
        for (const auto& e : all) {
            if (g_stop) break;
            const uint64_t due = start + uint64_t(double(e.rx - std::min(e.rx, rx0)) / speed);
            sleep_until_wall(due);
            if (g_stop) break;
            auto& s = ss[e.stream];
            auto loan = s.wr->Loan(e.bytes);
            if (!loan.ok()) { ++errors; continue; }
            std::fseek(s.f, e.offset, SEEK_SET);
            if (std::fread(loan.value.data, 1, e.bytes, s.f) != e.bytes) { ++errors; continue; }
            if (s.wr->Publish(std::move(loan.value)) != ::ara::rds::RdsError::OK) { ++errors; continue; }
            ++s.chunks;
            const uint64_t now = wall_ns();
            if (now > due) late_max_ns = std::max(late_max_ns, now - due);
        }
        start += span + 1000000ull;   // the next loop starts 1 ms after the last chunk
    } while (loop && !g_stop);
    std::printf("{\"mode\": \"replay\", \"dir\": \"%s\", \"errors\": %llu, \"late_max_ms\": %.3f, \"streams\": {",
                json_escape(dir).c_str(), (unsigned long long)errors, double(late_max_ns) / 1e6);
    for (size_t i = 0; i < ss.size(); ++i) {
        std::printf("%s\"%s\": {\"chunks\": %llu}", i ? ", " : "", json_escape(ss[i].path).c_str(),
                    (unsigned long long)ss[i].chunks);
        ss[i].wr->Shutdown();
        std::fclose(ss[i].f);
    }
    std::printf("}}\n");
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string mode = argv[1], dir = argv[2];
    double seconds = 0, speed = 1.0;
    uint32_t chunk = 0, start_delay_ms = 1000;
    uint64_t start_wall = 0, rx0 = 0;
    bool loop = false;
    std::vector<std::string> streams, only;
    int i = 3;
    if (mode == "record") {
        if (argc < 4) return usage();
        streams = split_csv(argv[3]);
        i = 4;
    }
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--loop") { loop = true; continue; }
        if (!(v = next())) return usage();
        if (a == "--seconds") seconds = std::atof(v);
        else if (a == "--chunk") chunk = uint32_t(std::strtoul(v, nullptr, 10));
        else if (a == "--speed") speed = std::atof(v);
        else if (a == "--start-wall-ns") start_wall = std::strtoull(v, nullptr, 10);
        else if (a == "--rx0-ns") rx0 = std::strtoull(v, nullptr, 10);
        else if (a == "--start-delay-ms") start_delay_ms = uint32_t(std::strtoul(v, nullptr, 10));
        else if (a == "--only") only = split_csv(v);
        else return usage();
    }
    if (speed <= 0) return usage();
    if (mode == "record") return cmd_record(dir, streams, seconds, chunk);
    if (mode == "replay") return cmd_replay(dir, speed, start_wall, rx0, start_delay_ms, only, loop);
    return usage();
}
