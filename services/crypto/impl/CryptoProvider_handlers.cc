// User handler bodies for CryptoProvider — the Crypto Service Manager.
//
// HAND-OWNED. Routes the two crypto surfaces to the OpenSSL SoftwareProvider
// (impl/software_provider.hpp):
//   CryptoProviderIf — the ara::crypto CONTEXT lifecycle (CreateCtx / CtxStart /
//                      CtxUpdate / CtxFinish) + SlotInfo. Streams large data in
//                      chunks via a server-side context handle.
//   CryptoOneShotIf  — Hash / Sign / Verify / GetCert in one call (small payloads).
// The PRIVATE KEY NEVER LEAVES this process — Sign/CtxFinish return only the
// signature; there is no export op.
//
// docs/tasks/PROGRESS/grpc-certificates.md (phase 2) + docs/autosar/services/
// crypto.md (the context API). Provider selection is trivial today (only the
// SoftwareProvider exists); phase 4 adds an HSM stub behind the same surface.

#include "lib/CryptoProvider.hh"

#include "impl/software_provider.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ParamsConfig.hh"   // get_config() — slot_dir param

namespace ara::crypto {

namespace {

SoftwareProvider& provider() {
    static SoftwareProvider p(
        ::theia::runtime::get_config()
            .node(CryptoProvider::kNodeName)
            .str("slot_dir", "/etc/theia/crypto"));
    return p;
}

// Copy a provider byte vector into a nanopb fixed bytes field. False (caller
// flags BACKEND_ERROR) on overflow.
template <typename BytesField>
bool set_bytes(BytesField& field, const std::vector<uint8_t>& src) {
    if (src.size() > sizeof(field.bytes)) return false;
    std::memcpy(field.bytes, src.data(), src.size());
    field.size = static_cast<pb_size_t>(src.size());
    return true;
}

template <typename Msg>
void set_msg(Msg& m, const std::string& s) {
    std::snprintf(m.message, sizeof(m.message), "%s", s.c_str());
}

}  // namespace

void CryptoProvider::init(CryptoProviderState& /*s*/) {
    log().info("crypto provider up (OpenSSL SoftwareProvider)");
}

void CryptoProvider::handle_info(const char* /*info*/, CryptoProviderState& /*s*/) {
}

// ==== context lifecycle (CryptoProviderIf) ================================

CreateCtxReply CryptoProvider::handle_call(const CreateCtxReq& req,
                                           CryptoProviderState& /*s*/) {
    CreateCtxReply rep = system_services_crypto_CreateCtxReply_init_zero;
    ProviderResult err;
    uint64_t h = provider().create(static_cast<int>(req.kind),
                                   static_cast<int>(req.algo),
                                   req.key_slot, err);
    if (h == 0) {
        rep.status = static_cast<system_services_crypto_CryptoStatus>(err.status);
        set_msg(rep, err.message);
        return rep;
    }
    rep.status = system_services_crypto_CryptoStatus_CryptoStatus_OK;
    rep.ctx = h;
    return rep;
}

CtxAck CryptoProvider::handle_call(const CtxStartReq& req,
                                   CryptoProviderState& /*s*/) {
    CtxAck rep = system_services_crypto_CtxAck_init_zero;
    auto r = provider().start(req.ctx);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    return rep;
}

CtxAck CryptoProvider::handle_call(const CtxUpdateReq& req,
                                   CryptoProviderState& /*s*/) {
    CtxAck rep = system_services_crypto_CtxAck_init_zero;
    auto r = provider().update(req.ctx, req.chunk.bytes, req.chunk.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    return rep;
}

CtxFinishReply CryptoProvider::handle_call(const CtxFinishReq& req,
                                           CryptoProviderState& /*s*/) {
    CtxFinishReply rep = system_services_crypto_CtxFinishReply_init_zero;
    auto r = provider().finish(req.ctx, req.signature.bytes, req.signature.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    rep.valid = r.valid;
    if (!r.bytes.empty() && !set_bytes(rep.output, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        set_msg(rep, "output overflow");
    }
    return rep;
}

SlotInfoReply CryptoProvider::handle_call(const SlotInfoReq& req,
                                          CryptoProviderState& /*s*/) {
    SlotInfoReply rep = system_services_crypto_SlotInfoReply_init_zero;
    std::string family;
    uint32_t usage = 0;
    bool exportable = false;
    if (!provider().slot_info(req.slot_id, family, usage, exportable)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_UNKNOWN_IDENTIFIER;
        set_msg(rep, "unknown slot");
        return rep;
    }
    rep.status = system_services_crypto_CryptoStatus_CryptoStatus_OK;
    rep.has_info = true;
    std::snprintf(rep.info.slot_id, sizeof(rep.info.slot_id), "%s",
                  req.slot_id);
    std::snprintf(rep.info.algorithm_family, sizeof(rep.info.algorithm_family),
                  "%s", family.c_str());
    rep.info.allowed_usage = usage;
    rep.info.exportable = exportable;
    return rep;
}

// ==== one-shot convenience (CryptoOneShotIf) =============================

HashReply CryptoProvider::handle_call(const HashReq& req,
                                      CryptoProviderState& /*s*/) {
    HashReply rep = system_services_crypto_HashReply_init_zero;
    auto r = provider().hash(static_cast<int>(req.algo), req.data.bytes,
                             req.data.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    if (r.status == 0 && !set_bytes(rep.digest, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        set_msg(rep, "digest overflow");
    }
    return rep;
}

SignReply CryptoProvider::handle_call(const SignReq& req,
                                      CryptoProviderState& /*s*/) {
    SignReply rep = system_services_crypto_SignReply_init_zero;
    auto r = provider().sign(req.key_slot, static_cast<int>(req.algo),
                             req.data.bytes, req.data.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    if (r.status == 0 && !set_bytes(rep.signature, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        set_msg(rep, "signature overflow");
    }
    return rep;
}

VerifyReply CryptoProvider::handle_call(const VerifyReq& req,
                                        CryptoProviderState& /*s*/) {
    VerifyReply rep = system_services_crypto_VerifyReply_init_zero;
    auto r = provider().verify(req.key_slot, static_cast<int>(req.algo),
                               req.data.bytes, req.data.size,
                               req.signature.bytes, req.signature.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    rep.valid = r.valid;
    return rep;
}

CertReply CryptoProvider::handle_call(const CertReq& req,
                                      CryptoProviderState& /*s*/) {
    CertReply rep = system_services_crypto_CertReply_init_zero;
    auto r = provider().get_cert(req.slot);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    if (r.status == 0 && !set_bytes(rep.pem, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        set_msg(rep, "pem overflow");
    }
    return rep;
}

// PrivateKeyOp(slot, input) → input^d mod n. The raw RSA private primitive the
// TLS engine proxies its handshake signing to — the key never leaves the FC.
PrivOpReply CryptoProvider::handle_call(const PrivOpReq& req,
                                        CryptoProviderState& /*s*/) {
    PrivOpReply rep = system_services_crypto_PrivOpReply_init_zero;
    auto r = provider().priv_op(req.slot, req.input.bytes, req.input.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    set_msg(rep, r.message);
    if (r.status == 0 && !set_bytes(rep.output, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        set_msg(rep, "output overflow");
    }
    return rep;
}

}  // namespace ara::crypto
