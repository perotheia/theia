// ebpf_loader — libbpf loader for the idsm_connect CO-RE program (Cat B v2).
//
// APP-OWNED, behind THEIA_HAVE_LIBBPF. Loads the compiled idsm_connect.bpf.o
// (path from config.bpf_object_path), attaches its sys_enter_connect tracepoint,
// and drains the ring buffer of conn_event records — the EDGE-TRIGGERED
// connect() sensor that catches a short-lived dial the userspace poll misses.
//
// libbpf 0.5 has no skeletons, so we use the generic object API
// (bpf_object__open_file / __load / find_program / attach) + ring_buffer__new /
// __poll. The ring callback appends to a vector the FC drains each tick.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

namespace ara::idsm {

// Mirror of struct conn_event in idsm_connect.bpf.c (wire-compatible layout).
struct ConnEvent {
    uint32_t pid;
    uint32_t family;
    uint32_t daddr4;     // network order
    uint16_t dport;      // host order
    char     comm[16];
};

class EbpfConnect {
public:
    ~EbpfConnect() { close(); }

    // Load + attach. Returns true on success (program live, ring open).
    bool open(const std::string& obj_path) {
        if (obj_path.empty()) return false;
        obj_ = bpf_object__open_file(obj_path.c_str(), nullptr);
        if (!obj_ || libbpf_get_error(obj_)) { obj_ = nullptr; return false; }
        if (bpf_object__load(obj_) != 0) { close(); return false; }

        struct bpf_program* prog =
            bpf_object__find_program_by_name(obj_, "idsm_connect");
        if (!prog) { close(); return false; }
        link_ = bpf_program__attach(prog);
        if (!link_ || libbpf_get_error(link_)) { link_ = nullptr; close(); return false; }

        int map_fd = bpf_object__find_map_fd_by_name(obj_, "events");
        if (map_fd < 0) { close(); return false; }
        rb_ = ring_buffer__new(map_fd, &EbpfConnect::on_sample_, this, nullptr);
        if (!rb_) { close(); return false; }
        return true;
    }

    bool live() const { return rb_ != nullptr; }

    // Drain pending events (non-blocking poll, 0ms timeout). Moves the collected
    // events out to the caller. Safe to call each tick.
    std::vector<ConnEvent> drain() {
        pending_.clear();
        if (rb_) ring_buffer__poll(rb_, 0 /*ms*/);
        return std::move(pending_);
    }

    void close() {
        if (rb_)   { ring_buffer__free(rb_); rb_ = nullptr; }
        if (link_) { bpf_link__destroy(link_); link_ = nullptr; }
        if (obj_)  { bpf_object__close(obj_); obj_ = nullptr; }
    }

private:
    struct bpf_object* obj_ = nullptr;
    struct bpf_link*   link_ = nullptr;
    struct ring_buffer* rb_ = nullptr;
    std::vector<ConnEvent> pending_;

    static int on_sample_(void* ctx, void* data, size_t len) {
        auto* self = static_cast<EbpfConnect*>(ctx);
        if (len >= sizeof(ConnEvent)) {
            ConnEvent e;
            std::memcpy(&e, data, sizeof(e));
            self->pending_.push_back(e);
        }
        return 0;
    }
};

}  // namespace ara::idsm
