// theia::runtime::PgDrops — a message the wire would not carry is never silent.
//
// A pg / cast message is DROPPED when it cannot be encoded (larger than the encode
// buffer, kMaxPgPayload), arrives truncated, or fails to decode. Each drop
// increments a per-message-type counter in this process; the FIRST drop of a type
// logs one warning (the type, why, the size); the process total rides every
// HeartbeatReport (pg_drops) to the supervisor, which logs each rise and carries it
// in NodeState. A type whose nanopb max size exceeds the limit does not build at all
// (PgClient's static_assert on RemoteCodec<T>::max_size).
#pragma once

#include "Logger.hh"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace theia { namespace runtime {

class PgDrops {
public:
    static PgDrops& get() noexcept { static PgDrops d; return d; }

    void note(const char* type, const char* why, size_t bytes) noexcept {
        total_.fetch_add(1, std::memory_order_relaxed);
        const std::string t = type ? type : "?";
        bool first = false;
        {
            std::lock_guard<std::mutex> l(mu_);
            first = (per_type_[t]++ == 0);
        }
        if (first)
            process_logger().warn("pg: DROPPED a " + t + " (" + (why ? why : "?") + ", " +
                                  std::to_string(bytes) + " B): further drops of this type are "
                                  "counted, not logged (HeartbeatReport.pg_drops, the "
                                  "supervisor's NodeState)");
    }
    uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
    std::map<std::string, uint64_t> per_type() const {
        std::lock_guard<std::mutex> l(mu_);
        return per_type_;
    }

private:
    std::atomic<uint64_t> total_{0};
    mutable std::mutex mu_;
    std::map<std::string, uint64_t> per_type_;
};

inline void note_pg_drop(const char* type, const char* why, size_t bytes) noexcept {
    PgDrops::get().note(type, why, bytes);
}

}}  // namespace theia::runtime
