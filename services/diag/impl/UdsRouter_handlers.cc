// UdsRouter — the UDS (ISO 14229) service dispatcher. HAND-OWNED.
//
// handle_call(UdsRequest) is the single inbound: DoipServer forwards the raw UDS
// bytes; we dispatch by service id, track session + security state, reach the
// backend FCs (per for DIDs, crypto for 0x27, phm for 0x19), and return the UDS
// response (positive, or a negative-response `7F sid nrc`). The DoIP transport
// is DoipServer's job; this node is pure UDS semantics.

#include "lib/UdsRouter.hh"
#include "impl/uds.hpp"           // UDS sids / NRCs / framing
#include "impl/crypto_link.hpp"   // 0x27 seed/key via services/crypto

#include <pb_decode.h>

#include <chrono>
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
    // S3 session timeout: revert to the default session + RE-LOCK security so a
    // dropped tester can't leave the ECU unlocked.
    if (info && std::strcmp(info, "s3_timeout") == 0) {
        if (s.session != uds::SESS_Default) {
            s.session = uds::SESS_Default;
            s.security_unlocked = false;
            s.seed_pending = false;
            this->log().info("S3 timeout — session → default, security re-locked");
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
    if (c.security_key_slot[0]) s.security_key_slot = c.security_key_slot;
    this->log().info(std::string("config: session_timeout_ms=") +
        std::to_string(s.session_timeout_ms) + " key_slot=" + s.security_key_slot);
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
    if (sub == uds::SESS_Default) { s.security_unlocked = false; s.seed_pending = false; }
    // Positive: sub + P2_server_max(2B) + P2*_server_max(2B) (50ms / 5000ms).
    uds::Bytes data = { sub, 0x00, 0x32, 0x01, 0xF4 };
    return uds::positive(uds::SID_DiagnosticSessionControl, data);
}

// 0x22 ReadDataByIdentifier: look up the DID (written overlay → static table).
// Phase-2 follow-up routes node-owned live DIDs via per / a TIPC call.
uds::Bytes svc_read_did(const uds::Bytes& req) {
    if (req.size() < 3) return uds::negative(uds::SID_ReadDataByIdentifier,
                                             uds::NRC_IncorrectMessageLength);
    uint16_t did = uds::did_of(req.data() + 1);
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

// 0x2E WriteDataByIdentifier: gated behind a passed 0x27. Stores into the
// written-DID overlay (a real build PutConfig's via per).
uds::Bytes svc_write_did(const uds::Bytes& req, UdsRouterState& s) {
    if (req.size() < 3) return uds::negative(uds::SID_WriteDataByIdentifier,
                                             uds::NRC_IncorrectMessageLength);
    if (!s.security_unlocked)
        return uds::negative(uds::SID_WriteDataByIdentifier,
                             uds::NRC_SecurityAccessDenied);
    uint16_t did = uds::did_of(req.data() + 1);
    written_dids()[did] = std::string(req.begin() + 3, req.end());
    uds::Bytes data;
    uds::put_did(data, did);       // positive response echoes the DID
    return uds::positive(uds::SID_WriteDataByIdentifier, data);
}

// 0x27 SecurityAccess. odd sub-function = requestSeed (return a seed); even =
// sendKey (the tester's signature over the seed) → crypto Verify. FAIL-CLOSED: a
// transport/verify failure leaves the session LOCKED. An exceeded attempt count
// returns NRC 0x36.
uds::Bytes svc_security(const uds::Bytes& req, UdsRouterState& s,
                        const std::string& key_slot) {
    if (req.size() < 2) return uds::negative(uds::SID_SecurityAccess,
                                             uds::NRC_IncorrectMessageLength);
    const uint8_t sub = req[1];

    if (sub == uds::SA_RequestSeed) {
        if (s.security_unlocked) {     // already unlocked → seed of zeros (ISO)
            uds::Bytes data = { sub, 0, 0, 0, 0 };
            return uds::positive(uds::SID_SecurityAccess, data);
        }
        // A 4-byte seed derived from a monotonic clock (v1; a real build pulls
        // crypto-strong randomness). Stash it for the sendKey check.
        uint64_t t = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        uint8_t seed[4] = { uint8_t(t), uint8_t(t >> 8), uint8_t(t >> 16),
                            uint8_t(t >> 24) };
        s.last_seed.assign(reinterpret_cast<char*>(seed), 4);
        s.seed_pending = true;
        uds::Bytes data = { sub, seed[0], seed[1], seed[2], seed[3] };
        return uds::positive(uds::SID_SecurityAccess, data);
    }

    if (sub == uds::SA_SendKey) {
        if (!s.seed_pending)
            return uds::negative(uds::SID_SecurityAccess, uds::NRC_ConditionsNotCorrect);
        if (s.security_attempts >= uds::kMaxSecurityAttempts)
            return uds::negative(uds::SID_SecurityAccess,
                                 uds::NRC_ExceededNumberOfAttempts);
        std::string key(req.begin() + 2, req.end());
        bool valid = false;
        bool ok = CryptoLink::instance().verify(key_slot, s.last_seed, key, valid);
        s.seed_pending = false;
        if (ok && valid) {             // crypto confirmed the signature
            s.security_unlocked = true;
            s.security_attempts = 0;
            uds::Bytes data = { sub };
            return uds::positive(uds::SID_SecurityAccess, data);
        }
        // FAIL-CLOSED: transport error OR invalid key → stay locked, count it.
        s.security_attempts += 1;
        return uds::negative(uds::SID_SecurityAccess, uds::NRC_InvalidKey);
    }

    return uds::negative(uds::SID_SecurityAccess, uds::NRC_SubFunctionNotSupported);
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
    case uds::SID_ReadDataByIdentifier:     resp = svc_read_did(in);    break;
    case uds::SID_WriteDataByIdentifier:    resp = svc_write_did(in, s); break;
    case uds::SID_SecurityAccess:
        resp = svc_security(in, s, s.security_key_slot);  break;
    case uds::SID_ReadDTCInformation:       // Phase 4
        resp = uds::negative(sid, uds::NRC_ServiceNotSupported);
        break;
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
