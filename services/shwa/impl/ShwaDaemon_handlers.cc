// User handler bodies for ShwaDaemon — the Safe-Hardware-Accelerator node.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// On a timer tick ShwaDaemon samples the build-selected backend (host or
// jetson), broadcasts an AccelTelemetry record (CPU/GPU/mem/temp/power/fan),
// reconciles the Orin power mode, and serves GetAccelStatus / SetPowerMode. The
// power plane (nvpmodel/jetson_clocks) lives HERE now — moved from osi, where it
// was misplaced. Read-only telemetry + the power knob; never a data path.

#include "lib/ShwaDaemon.hh"
#include "impl/shwa_backend.hpp"

#include "TimerService.hh"      // post_info / send_after / process_timers
#include "TheiaMsgHeader.hh"    // TheiaMsgHeader + kBusTypeRpc / kMsgGenCast
#include "RemoteCodec.hh"       // hash_msg_type_ (AccelSample service_id)

#include <pb_decode.h>
#include <pb_encode.h>

#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ara::shwa {

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Fill an AccelSample wire message from a backend AccelReading.
AccelSample to_wire_(const AccelReading& r) {
    AccelSample m = system_services_shwa_AccelSample_init_zero;
    std::strncpy(m.board, r.board.c_str(), sizeof(m.board) - 1);
    m.on_jetson    = r.on_jetson;
    m.power_mode   = static_cast<system_services_shwa_PowerMode>(r.power_mode);
    m.cpu_util_pct = r.cpu_util_pct;
    m.cpu_count    = r.cpu_count;
    m.cpu_freq_mhz = r.cpu_freq_mhz;
    m.gpu_util_pct = r.gpu_util_pct;
    m.gpu_freq_mhz = r.gpu_freq_mhz;
    m.mem_used_mb  = r.mem_used_mb;
    m.mem_total_mb = r.mem_total_mb;
    m.temp_c       = r.temp_c;
    m.power_mw     = r.power_mw;
    m.fan_rpm      = r.fan_rpm;
    m.uptime_sec             = r.uptime_sec;
    m.disk_root_total_kb     = r.disk_root_total_kb;
    m.disk_root_avail_kb     = r.disk_root_avail_kb;
    m.disk_install_total_kb  = r.disk_install_total_kb;
    m.disk_install_avail_kb  = r.disk_install_avail_kb;
    m.ts_ns        = now_ns_();
    return m;
}

// ── AccelTelemetry egress over TIPC (SHWA → com) ─────────────────────────────
//
// AccelTelemetry is a senderReceiver broadcast; the runtime's generated
// broadcast_* fan-out is IN-PROCESS only (subscriber callbacks). To reach com
// (the gRPC bridge to the GUI) — a separate process — SHWA hand-frames each
// AccelSample as a GEN_CAST and SOCK_DGRAM-sendto's a well-known AccelTelemetry
// egress TIPC service name. This MIRRORS the runtime's TraceSubmitter (the trace
// egress producer, Tracer.hh): connectionless, best-effort, drop-on-block — a
// slow/absent com must never back-pressure the poll thread, and a dropped
// telemetry sample is fine (the next tick re-sends). com binds this name and
// gates forwarding into the gRPC stream on a connected subscriber.
class AccelSubmitter {
public:
    // The AccelTelemetry egress channel. com (shwa_link) binds this DGRAM name.
    // Free address near SHWA's own 0x80010012 (verified against the manifest).
    static constexpr uint32_t kEgressTipcType     = 0x8001001Au;
    static constexpr uint32_t kEgressTipcInstance = 0u;

    // service_id com's receiver matches: djb2_low16 of the nanopb C type name,
    // the SAME hash RemoteCodec<AccelSample> computes — derived so it can't drift.
    static constexpr const char* kRecordTypeName =
        "system_services_shwa_AccelSample";
    static constexpr uint16_t kRecordServiceId =
        ::theia::runtime::hash_msg_type_(kRecordTypeName);

    static AccelSubmitter& instance() { static AccelSubmitter s; return s; }

    // Encode `m` to proto-wire, frame [TheiaMsgHeader|GEN_CAST][AccelSample],
    // and sendto the egress name. Lossy by design — returns void.
    void submit(const AccelSample& m) noexcept {
        uint8_t pb[256];
        pb_ostream_t os = pb_ostream_from_buffer(pb, sizeof(pb));
        if (!pb_encode(&os, system_services_shwa_AccelSample_fields, &m)) return;

        std::lock_guard<std::mutex> lk(mu_);
        if (!ensure_open_locked()) return;
        ::theia::runtime::TheiaMsgHeader hdr{};
        hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type           = ::theia::runtime::kMsgGenCast;
        hdr.proto_len          = static_cast<uint16_t>(os.bytes_written);
        hdr.rpc.service_id     = kRecordServiceId;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        std::vector<uint8_t> frame(sizeof(hdr) + os.bytes_written);
        std::memcpy(frame.data(), &hdr, sizeof(hdr));
        std::memcpy(frame.data() + sizeof(hdr), pb, os.bytes_written);
        (void)::sendto(fd_, frame.data(), frame.size(),
                       MSG_NOSIGNAL | MSG_DONTWAIT,
                       reinterpret_cast<struct sockaddr*>(&addr_), sizeof(addr_));
    }

private:
    AccelSubmitter() {
        addr_.family                  = AF_TIPC;
        addr_.addrtype                = TIPC_ADDR_NAME;
        addr_.addr.name.name.type     = kEgressTipcType;
        addr_.addr.name.name.instance = kEgressTipcInstance;
        addr_.scope                   = TIPC_NODE_SCOPE;
    }
    ~AccelSubmitter() { if (fd_ >= 0) ::close(fd_); }

    bool ensure_open_locked() noexcept {
        if (fd_ >= 0) return true;
        fd_ = ::socket(AF_TIPC, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        return fd_ >= 0;
    }

    std::mutex           mu_;
    int                  fd_ = -1;
    struct sockaddr_tipc addr_{};
};

}  // namespace

// init: bring up the backend, apply config, kick off the poll loop.
void ShwaDaemon::init(ShwaDaemonState& s) {
    backend::init();
    s.started = true;
    s.power_mode = backend::read_power_mode();   // initial read (UNKNOWN off-Jetson)
    backend::sample(s.last);                     // a first reading for GetAccelStatus
    log().info(std::string("shwa up — hardware telemetry + power "
        "(board=") + s.last.board + ", on_jetson=" +
        (backend::on_jetson() ? "yes" : "no") + ")");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": sample, broadcast, reconcile power, reschedule.
void ShwaDaemon::handle_info(const char* info, ShwaDaemonState& s) {
    if (!info || std::strcmp(info, "poll") != 0) return;

    backend::sample(s.last);
    AccelSample wire = to_wire_(s.last);
    broadcast_broadcast_sample(wire);          // in-process subscribers
    AccelSubmitter::instance().submit(wire);   // TIPC egress → com (GUI bridge)

    // Reconcile the desired power mode on Orin (edge — only on a change).
    if (backend::on_jetson() && s.power_mode != s.applied_power_mode &&
        s.power_mode != PM_UNKNOWN) {
        std::string err;
        if (backend::apply_power_mode(s.power_mode, s.jetson_clocks, err)) {
            s.applied_power_mode = s.power_mode;
            log().info("power mode → " + std::to_string(s.power_mode) +
                       (s.jetson_clocks ? " (+jetson_clocks)" : ""));
        } else {
            log().warn("power mode apply failed: " + err);
        }
    }

    uint32_t ms = s.poll_ms ? s.poll_ms : 2000;
    ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                 *this, "poll");
}

// on_config_update: apply ShwaConfig live (cadence, desired power mode).
void ShwaDaemon::on_config_update(const platform_runtime_ConfigUpdated& cfg,
                                  ShwaDaemonState& s) {
    system_services_shwa_ShwaConfig c = system_services_shwa_ShwaConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_shwa_ShwaConfig_fields, &c)) {
        log().warn("on_config_update: ShwaConfig decode failed — not applied");
        return;
    }
    s.poll_ms       = c.poll_ms ? c.poll_ms : 2000;
    s.power_mode    = static_cast<int>(c.power_mode);
    s.jetson_clocks = c.jetson_clocks;
    s.persist       = c.persist;
    log().info(std::string("config: poll_ms=") + std::to_string(s.poll_ms) +
        " power_mode=" + std::to_string(s.power_mode) +
        " jetson_clocks=" + (s.jetson_clocks ? "true" : "false"));
}

// GetAccelStatus — serve the latest sample.
AccelSample ShwaDaemon::handle_call(const AccelStatusReq& /*req*/,
                                    ShwaDaemonState& s) {
    return to_wire_(s.last);
}

// SetPowerMode — set the Orin power profile (no-op + applied=false off-Jetson).
PowerModeReply ShwaDaemon::handle_call(const PowerModeReq& req,
                                       ShwaDaemonState& s) {
    PowerModeReply rep = system_services_shwa_PowerModeReply_init_zero;
    std::string err;
    if (!backend::on_jetson()) {
        rep.applied = false;
        std::strncpy(rep.message, "not a Jetson — power mode unmanaged",
                     sizeof(rep.message) - 1);
        return rep;
    }
    bool ok = backend::apply_power_mode(static_cast<int>(req.mode),
                                        req.jetson_clocks, err);
    if (ok) {
        s.power_mode = static_cast<int>(req.mode);
        s.applied_power_mode = s.power_mode;
        s.jetson_clocks = req.jetson_clocks;
        std::strncpy(rep.message, "applied", sizeof(rep.message) - 1);
    } else {
        std::strncpy(rep.message, err.c_str(), sizeof(rep.message) - 1);
    }
    rep.applied = ok;
    return rep;
}

}  // namespace ara::shwa
