// UdsRouter — the UDS (ISO 14229) service dispatcher. HAND-OWNED.
//
// handle_call(UdsRequest) is the single inbound: DoipServer forwards the raw UDS
// bytes; we dispatch by service id, track session state, reach the backend FCs
// (per for DIDs, phm for 0x19 DTC + fault-log DIDs), and return the UDS response
// (positive, or a negative-response `7F sid nrc`). The DoIP transport is
// DoipServer's job; this node is pure UDS semantics.

#include "lib/UdsRouter.hh"
#include "impl/uds.hpp"           // UDS sids / NRCs / framing
#include "impl/phm_link.hpp"      // 0x19 ReadDTC + fault-log DIDs via services/phm

#include <pb_decode.h>


#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ara::diag {

namespace {

// v1 static DID table (read-only). VIN (0xF190) + a couple of identification
// DIDs. Phase-2 follow-up: read these from services/per (GetConfig) + a per-DID
// live-route table (TIPC call into the owning FC). For now a compiled baseline so
// the 0x22 path is exercisable end-to-end.
const std::map<uint16_t, std::string>& static_dids() {
    static const std::map<uint16_t, std::string> t = {
        {0xF190, "THEIA0DIAG0000001"},   // VIN (0xF190 = VehicleIdentificationNumber)
        {0xF195, "1.0.0"},               // System Supplier ECU SW Version
        {0xF187, "THEIA-DIAG"},          // Vehicle Manufacturer Spare Part Number
    };
    return t;
}

// Mutable DID store for 0x2E writes (overlays the static table). Process-global
// (one router instance). A real build persists via per.
std::map<uint16_t, std::string>& written_dids() {
    static std::map<uint16_t, std::string> w;
    return w;
}

uds::Bytes to_reply(const uds::Bytes& uds_bytes, bool& is_nrc) {
    is_nrc = uds::is_negative(uds_bytes);
    return uds_bytes;
}

}  // namespace

void UdsRouter::init(UdsRouterState& s) {
    this->log().info("uds router up — UDS dispatch ready (session=default, locked)");
    (void)s;
}

void UdsRouter::handle_info(const char* info, UdsRouterState& s) {
    // S3 session timeout: revert to the default session if the tester went quiet.
    if (info && std::strcmp(info, "s3_timeout") == 0) {
        if (s.session != uds::SESS_Default) {
            s.session = uds::SESS_Default;
            this->log().info("S3 timeout — session → default");
        }
    }
}

void UdsRouter::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        UdsRouterState& s) {
    system_services_diag_DiagConfig c = system_services_diag_DiagConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_diag_DiagConfig_fields, &c)) {
        this->log().warn("on_config_update: DiagConfig decode failed — ignored");
        return;
    }
    if (c.session_timeout_ms) s.session_timeout_ms = c.session_timeout_ms;
    this->log().info(std::string("config: session_timeout_ms=") +
        std::to_string(s.session_timeout_ms));
}

// ---- PhmDtcStream: phm casts a PhmFaultEvent on every fault transition.
//      Append it to the running fault LOG (ring-capped). 0x22 fault-log DIDs
//      read this by index; 0x19 reports it as DTCs. This is the "DIDs from phm,
//      a running log with a fault idx" source — no static DID table.
void UdsRouter::handle_cast(const PhmFaultEvent& msg, UdsRouterState& s) {
    UdsRouterState::FaultRec r;
    r.entity = msg.entity;
    r.level  = static_cast<uint32_t>(msg.level);
    r.kind   = static_cast<uint32_t>(msg.kind);
    r.ts_ns  = msg.ts_ns;
    if (s.fault_log.size() >= UdsRouterState::kMaxFaults)
        s.fault_log.erase(s.fault_log.begin());   // ring: drop the oldest
    s.fault_log.push_back(std::move(r));
    this->log().info(std::string("fault log += ") + msg.entity + " level=" +
        std::to_string((unsigned)msg.level) + " (idx " +
        std::to_string(s.fault_log.size() - 1) + ")");
}

// ── Per-service handlers (return UDS response bytes) ─────────────────────────

namespace {

// 0x10 DiagnosticSessionControl: switch session; positive response echoes the
// sub-function + the P2/P2* timing record (fixed for v1).
uds::Bytes svc_session(const uds::Bytes& req, UdsRouterState& s) {
    if (req.size() < 2) return uds::negative(uds::SID_DiagnosticSessionControl,
                                             uds::NRC_IncorrectMessageLength);
    uint8_t sub = req[1];
    if (sub != uds::SESS_Default && sub != uds::SESS_Programming &&
        sub != uds::SESS_Extended)
        return uds::negative(uds::SID_DiagnosticSessionControl,
                             uds::NRC_SubFunctionNotSupported);
    s.session = sub;
    // (0x27 dropped — no security state to reset on default session.)
    // Positive: sub + P2_server_max(2B) + P2*_server_max(2B) (50ms / 5000ms).
    uds::Bytes data = { sub, 0x00, 0x32, 0x01, 0xF4 };
    return uds::positive(uds::SID_DiagnosticSessionControl, data);
}

// DID ranges:
//   0xFD00 .. 0xFD3F  FAULT-LOG DIDs — the Nth (idx = DID - 0xFD00) entry of the
//                     running phm fault log (entity + level + kind + ts). The
//                     "fault idx" surfaced over UDS. NRC 0x31 past the end.
//   0xFDFF            fault COUNT (how many faults are logged).
//   else              identity DIDs (VIN etc.) from the written overlay / static
//                     baseline (a real build reads these from per/etcd config).
constexpr uint16_t kFaultDidBase  = 0xFD00;
constexpr uint16_t kFaultDidTop   = 0xFD3F;
constexpr uint16_t kFaultCountDid = 0xFDFF;

// 0x22 ReadDataByIdentifier. Fault-log DIDs read the phm fault log by index;
// identity DIDs read the static/written table.
uds::Bytes svc_read_did(const uds::Bytes& req, const UdsRouterState& s) {
    if (req.size() < 3) return uds::negative(uds::SID_ReadDataByIdentifier,
                                             uds::NRC_IncorrectMessageLength);
    const uint16_t did = uds::did_of(req.data() + 1);

    // Fault count.
    if (did == kFaultCountDid) {
        uds::Bytes data; uds::put_did(data, did);
        data.push_back(uint8_t(s.fault_log.size()));
        return uds::positive(uds::SID_ReadDataByIdentifier, data);
    }
    // Fault-log entry by index.
    if (did >= kFaultDidBase && did <= kFaultDidTop) {
        const size_t idx = did - kFaultDidBase;
        if (idx >= s.fault_log.size())
            return uds::negative(uds::SID_ReadDataByIdentifier,
                                 uds::NRC_RequestOutOfRange);
        const auto& f = s.fault_log[idx];
        // Record: level(1) + kind(1) + ts_ns(8 BE) + entity (ASCII).
        uds::Bytes data; uds::put_did(data, did);
        data.push_back(uint8_t(f.level));
        data.push_back(uint8_t(f.kind));
        for (int i = 7; i >= 0; --i) data.push_back(uint8_t(f.ts_ns >> (i * 8)));
        data.insert(data.end(), f.entity.begin(), f.entity.end());
        return uds::positive(uds::SID_ReadDataByIdentifier, data);
    }

    // Identity DIDs (written overlay → static baseline).
    auto wi = written_dids().find(did);
    const std::string* val = nullptr;
    if (wi != written_dids().end()) val = &wi->second;
    else {
        auto si = static_dids().find(did);
        if (si != static_dids().end()) val = &si->second;
    }
    if (!val) return uds::negative(uds::SID_ReadDataByIdentifier,
                                   uds::NRC_RequestOutOfRange);
    uds::Bytes data;
    uds::put_did(data, did);
    data.insert(data.end(), val->begin(), val->end());
    return uds::positive(uds::SID_ReadDataByIdentifier, data);
}

// 0x2E WriteDataByIdentifier. Stores into the written-DID overlay (a real build
// PutConfig's via per). (0x27 SecurityAccess was dropped — write is not gated.)
uds::Bytes svc_write_did(const uds::Bytes& req) {
    if (req.size() < 3) return uds::negative(uds::SID_WriteDataByIdentifier,
                                             uds::NRC_IncorrectMessageLength);
    uint16_t did = uds::did_of(req.data() + 1);
    written_dids()[did] = std::string(req.begin() + 3, req.end());
    uds::Bytes data;
    uds::put_did(data, did);       // positive response echoes the DID
    return uds::positive(uds::SID_WriteDataByIdentifier, data);
}


// 0x19 ReadDTCInformation. v1 sub-functions:
//   0x02 reportDTCByStatusMask: query phm health; if worst >= DEGRADED emit one
//        aggregate DTC (code 0xD10000 "platform degraded") with the testFailed +
//        confirmed status bits, else an empty list. Format: report-type echo +
//        status-availability-mask + [DTC(3B) + status(1B)]*.
//   0x0A reportSupportedDTC: the static set of DTCs this ECU can report.
uds::Bytes svc_read_dtc(const uds::Bytes& req) {
    if (req.size() < 2) return uds::negative(uds::SID_ReadDTCInformation,
                                             uds::NRC_IncorrectMessageLength);
    const uint8_t sub = req[1];
    // status availability mask (which status bits this ECU supports): testFailed
    // (0x01) | confirmedDTC (0x08) | testFailedSinceLastClear (0x20).
    constexpr uint8_t kStatusAvail = 0x29;
    // The one platform DTC v1 reports: 0xD1 0x00 0x00.
    const uint8_t dtc[3] = { 0xD1, 0x00, 0x00 };

    if (sub == uds::DTC_ReportByStatusMask) {
        uint32_t worst = 0, n_ent = 0, n_deg = 0;
        bool ok = PhmLink::instance().health(worst, n_ent, n_deg);
        uds::Bytes data = { sub, kStatusAvail };
        if (ok && worst >= 2u /*DEGRADED*/) {
            // status = testFailed(0x01) | confirmedDTC(0x08) [| testFailedSince…].
            uint8_t status = 0x09;
            if (worst >= 3u /*FAILED*/) status |= 0x20;
            data.push_back(dtc[0]); data.push_back(dtc[1]); data.push_back(dtc[2]);
            data.push_back(status);
        }
        return uds::positive(uds::SID_ReadDTCInformation, data);
    }

    if (sub == uds::DTC_ReportSupported) {
        // report-type echo + status-availability + the supported DTC list (status
        // 0x00 = "supported, currently inactive").
        uds::Bytes data = { sub, kStatusAvail,
                            dtc[0], dtc[1], dtc[2], 0x00 };
        return uds::positive(uds::SID_ReadDTCInformation, data);
    }

    return uds::negative(uds::SID_ReadDTCInformation, uds::NRC_SubFunctionNotSupported);
}

}  // namespace

// The single inbound. Dispatch by service id; unknown → serviceNotSupported.
// (0x27 SecurityAccess + 0x19 ReadDTC are wired in Phase 3/4 — they return a
// not-yet-supported NRC here so the negative-response path is still exercised.)
UdsReply UdsRouter::handle_call(
        const UdsRequest& req,
        UdsRouterState& s) {
    uds::Bytes in(req.uds.bytes, req.uds.bytes + req.uds.size);
    UdsReply reply = system_services_diag_UdsReply_init_zero;
    if (in.empty()) {
        uds::Bytes nrc = uds::negative(0x00, uds::NRC_GeneralReject);
        std::memcpy(reply.uds.bytes, nrc.data(), nrc.size());
        reply.uds.size = static_cast<pb_size_t>(nrc.size());
        reply.is_nrc = true;
        return reply;
    }
    const uint8_t sid = in[0];
    uds::Bytes resp;
    switch (sid) {
    case uds::SID_DiagnosticSessionControl: resp = svc_session(in, s);  break;
    case uds::SID_ReadDataByIdentifier:     resp = svc_read_did(in, s);  break;
    case uds::SID_WriteDataByIdentifier:    resp = svc_write_did(in);   break;
    case uds::SID_ReadDTCInformation:       resp = svc_read_dtc(in);    break;
    default:
        resp = uds::negative(sid, uds::NRC_ServiceNotSupported);
        break;
    }

    bool is_nrc = false;
    resp = to_reply(resp, is_nrc);
    if (resp.size() > sizeof(reply.uds.bytes)) resp.resize(sizeof(reply.uds.bytes));
    std::memcpy(reply.uds.bytes, resp.data(), resp.size());
    reply.uds.size = static_cast<pb_size_t>(resp.size());
    reply.is_nrc = is_nrc;

    this->log().info(std::string("UDS sid=0x") +
        [](uint8_t v){ char b[3]; std::snprintf(b, 3, "%02X", v); return std::string(b); }(sid) +
        (is_nrc ? " → NRC" : " → positive"));
    return reply;
}

}  // namespace ara::diag
