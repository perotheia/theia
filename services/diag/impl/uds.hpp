// uds.hpp — UDS (ISO 14229) service ids, sub-functions, negative-response codes,
// and response framing helpers. APP-OWNED, header-only + in-process (testable).
//
// A UDS request is `sid [sub-function] [data...]`; a positive response is
// `(sid|0x40) [data...]`; a negative response is `0x7F sid nrc`. This header
// holds the constants + the framing; the per-service logic lives in
// UdsRouter_handlers.cc.

#pragma once

#include <cstdint>
#include <vector>

namespace ara::diag {
namespace uds {

using Bytes = std::vector<uint8_t>;

// ── Service identifiers (the v1 set) ────────────────────────────────────────
enum Sid : uint8_t {
    SID_DiagnosticSessionControl = 0x10,
    SID_ReadDataByIdentifier     = 0x22,
    SID_SecurityAccess           = 0x27,
    SID_WriteDataByIdentifier    = 0x2E,
    SID_ReadDTCInformation       = 0x19,
    SID_TesterPresent            = 0x3E,   // the S3_server keep-alive
};
// TesterPresent sub-function 0x00 + the suppressPosRspMsgIndicationBit form.
constexpr uint8_t kTpZeroSubFunction  = 0x00;
constexpr uint8_t kSuppressPosRspBit  = 0x80;
constexpr uint8_t kPositiveOffset = 0x40;   // response sid = request sid | 0x40
constexpr uint8_t kNegativeResp   = 0x7F;

// ── Negative-response codes (ISO 14229 Annex A) ─────────────────────────────
enum Nrc : uint8_t {
    NRC_GeneralReject              = 0x10,
    NRC_ServiceNotSupported        = 0x11,
    NRC_SubFunctionNotSupported    = 0x12,
    NRC_IncorrectMessageLength     = 0x13,
    NRC_ConditionsNotCorrect       = 0x22,
    NRC_RequestOutOfRange          = 0x31,   // unknown DID
    NRC_SecurityAccessDenied       = 0x33,   // write without 0x27
    NRC_InvalidKey                 = 0x35,
    NRC_ExceededNumberOfAttempts   = 0x36,
    NRC_RequiredTimeDelayNotExpired = 0x37,
};

// Session ids (0x10 sub-function).
enum Session : uint8_t {
    SESS_Default     = 0x01,
    SESS_Programming = 0x02,
    SESS_Extended    = 0x03,
};

// SecurityAccess sub-functions (0x27): odd = requestSeed, even = sendKey.
constexpr uint8_t SA_RequestSeed = 0x01;
constexpr uint8_t SA_SendKey     = 0x02;
constexpr uint32_t kMaxSecurityAttempts = 3;

// ReadDTCInformation sub-functions (0x19).
constexpr uint8_t DTC_ReportByStatusMask = 0x02;
constexpr uint8_t DTC_ReportSupported    = 0x0A;

// ── Framing ─────────────────────────────────────────────────────────────────
inline Bytes positive(uint8_t sid) { return Bytes{ uint8_t(sid | kPositiveOffset) }; }
inline Bytes positive(uint8_t sid, const Bytes& data) {
    Bytes b{ uint8_t(sid | kPositiveOffset) };
    b.insert(b.end(), data.begin(), data.end());
    return b;
}
inline Bytes negative(uint8_t sid, uint8_t nrc) {
    return Bytes{ kNegativeResp, sid, nrc };
}
inline bool is_negative(const Bytes& resp) {
    return resp.size() >= 1 && resp[0] == kNegativeResp;
}

inline uint16_t did_of(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
inline void put_did(Bytes& b, uint16_t did) {
    b.push_back(uint8_t(did >> 8)); b.push_back(uint8_t(did));
}

}  // namespace uds
}  // namespace ara::diag
