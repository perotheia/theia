// SupFirehose reassembly + fan-out. See sup_firehose.hpp.
//
// Builds a LIBPROTOBUF TreeSnapshot from the #429 topo-pair stream. The
// libprotobuf TreeSnapshot has no 64 kB limit (it's heap-backed) — the limit
// the stream removes was nanopb's fixed-struct ceiling ON THE WIRE. So
// reassembling here for the gRPC face is fine and keeps the gRPC oneof stable.

#include "impl/sup_firehose.hpp"

// libprotobuf supervisor message types (the gRPC-edge codec).
#include "ChildState.pb.h"
#include "TreeSnapshot.pb.h"

#include <map>
#include <utility>

namespace services_com {

// Reassembly state — name-keyed ChildState rows accumulated between
// SnapshotBegin and SnapshotEnd, preserving topo (insertion) order so the
// rebuilt TreeSnapshot's child list matches the supervisor's walk.
struct SupFirehose::Build {
    uint64_t generation = 0;
    uint64_t timestamp_ms = 0;
    std::vector<std::string> order;                 // names, topo order
    std::map<std::string, ::services::supervisor::ChildState> rows;

    ::services::supervisor::ChildState& row(const std::string& name) {
        auto it = rows.find(name);
        if (it == rows.end()) {
            order.push_back(name);
            auto& r = rows[name];
            r.set_name(name);
            return r;
        }
        return it->second;
    }
};

SupFirehose& SupFirehose::instance() {
    static SupFirehose g;
    return g;
}

std::shared_ptr<Subscriber> SupFirehose::subscribe() {
    auto s = std::make_shared<Subscriber>();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    subs_.push_back(s);
    return s;
}

void SupFirehose::unsubscribe(const std::shared_ptr<Subscriber>& s) {
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->closed = true;
    }
    s->cv.notify_all();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    for (auto it = subs_.begin(); it != subs_.end();) {
        auto sp = it->lock();
        if (!sp || sp == s) it = subs_.erase(it);
        else ++it;
    }
}

void SupFirehose::push_frame_(uint16_t tag, std::string payload) {
    std::lock_guard<std::mutex> lk(sub_mtx_);
    for (auto it = subs_.begin(); it != subs_.end();) {
        auto sp = it->lock();
        if (!sp) { it = subs_.erase(it); continue; }
        {
            std::lock_guard<std::mutex> slk(sp->mtx);
            sp->queue.push_back(Frame{tag, payload});
        }
        sp->cv.notify_one();
        ++it;
    }
}

void SupFirehose::on_snapshot_begin(uint64_t generation, uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lk(build_mtx_);
    build_ = std::make_unique<Build>();
    build_->generation = generation;
    build_->timestamp_ms = timestamp_ms;
    open_generation_ = generation;
    in_snapshot_ = true;
}

void SupFirehose::on_node_edge(uint32_t op, const std::string& parent_name,
                               const std::string& name, uint32_t kind) {
    std::lock_guard<std::mutex> lk(build_mtx_);
    if (!in_snapshot_ || !build_) return;     // edge outside a walk — ignore
    if (op == 1) {                            // REMOVE
        build_->rows.erase(name);
        for (auto it = build_->order.begin(); it != build_->order.end(); ++it) {
            if (*it == name) { build_->order.erase(it); break; }
        }
        return;
    }
    auto& r = build_->row(name);              // ADD (op == 0)
    r.set_parent_name(parent_name);
    r.set_kind(kind);
}

void SupFirehose::on_node_state(
        const std::string& name, int32_t pid, uint32_t tid,
        uint32_t state, uint32_t flags, uint32_t restart_count,
        int32_t last_exit_code, uint64_t uptime_ms,
        uint32_t cpu_pct, uint64_t rss_kb, uint64_t vsz_kb,
        uint32_t threads, uint64_t shared_kb, uint64_t data_kb) {
    std::lock_guard<std::mutex> lk(build_mtx_);
    if (!in_snapshot_ || !build_) return;     // state outside a walk — ignore
    (void)tid;  // tid not carried on the libprotobuf ChildState (per-thread
                // detail lives on the GetTree path); the GUI keys on pid.
    auto& r = build_->row(name);
    r.set_pid(pid);
    r.set_state(state);
    r.set_flags(flags);
    r.set_restart_count(restart_count);
    r.set_last_exit_code(last_exit_code);
    r.set_uptime_ms(uptime_ms);
    r.set_cpu_pct(cpu_pct);
    r.set_rss_kb(rss_kb);
    r.set_vsz_kb(vsz_kb);
    r.set_threads(threads);
    r.set_shared_kb(shared_kb);
    r.set_data_kb(data_kb);
}

void SupFirehose::on_snapshot_end(uint64_t generation) {
    std::string payload;
    {
        std::lock_guard<std::mutex> lk(build_mtx_);
        if (!build_ || build_->generation != generation) {
            // Mismatched / missing Begin — drop the partial build.
            in_snapshot_ = false;
            build_.reset();
            return;
        }
        ::services::supervisor::TreeSnapshot snap;
        snap.set_generation(build_->generation);
        snap.set_timestamp_ms(build_->timestamp_ms);
        for (const auto& nm : build_->order) {
            auto it = build_->rows.find(nm);
            if (it != build_->rows.end()) *snap.add_children() = it->second;
        }
        payload = snap.SerializeAsString();
        in_snapshot_ = false;
        build_.reset();
    }
    // Fan out the reassembled snapshot as the SAME tagged Frame the gRPC
    // Subscribe handler already consumes.
    push_frame_(kTagSnapshot, std::move(payload));
}

}  // namespace services_com
