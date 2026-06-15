// User handler bodies for CryptoProvider — the Crypto Service Manager.
//
// HAND-OWNED. Each handle_call dispatches a CryptoProviderIf op (Hash / Sign /
// Verify / GetCert) to the OpenSSL SoftwareProvider (impl/software_provider.hpp).
// The provider holds the slot→PEM material; the PRIVATE KEY NEVER LEAVES this
// process — Sign returns only a signature, and there is no export op.
//
// Phase 2 of docs/tasks/PROGRESS/grpc-certificates.md. The provider is selected
// trivially today (only SoftwareProvider exists); phase 4 adds a HardwareProvider
// stub behind the same surface + a slot→provider routing step.

#include "lib/CryptoProvider.hh"

#include "impl/software_provider.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ParamsConfig.hh"   // get_config() — slot_dir param

namespace ara::crypto {

namespace {

// Process-global provider (the node is single-instance). slot_dir comes from the
// node's params (config/crypto.json: crypto_provider.slot_dir), default
// /etc/theia/crypto. Built lazily on first use so init order doesn't matter.
SoftwareProvider& provider() {
    static SoftwareProvider p(
        ::theia::runtime::get_config()
            .node(CryptoProvider::kNodeName)
            .str("slot_dir", "/etc/theia/crypto"));
    return p;
}

// Copy a provider byte vector into a nanopb fixed bytes field (size pinned in
// crypto.options). Returns false (caller flags BACKEND_ERROR) on overflow.
template <typename BytesField>
bool set_bytes(BytesField& field, const std::vector<uint8_t>& src) {
    if (src.size() > sizeof(field.bytes)) return false;
    std::memcpy(field.bytes, src.data(), src.size());
    field.size = static_cast<pb_size_t>(src.size());
    return true;
}

}  // namespace

void CryptoProvider::init(CryptoProviderState& /*s*/) {
    log().info("crypto provider up (OpenSSL SoftwareProvider)");
}

void CryptoProvider::handle_info(const char* /*info*/, CryptoProviderState& /*s*/) {
}

// Hash(data) → digest.
HashReply CryptoProvider::handle_call(const HashReq& req,
                                      CryptoProviderState& /*s*/) {
    HashReply rep = system_services_crypto_HashReply_init_zero;
    auto r = provider().hash(static_cast<int>(req.algo), req.data.bytes,
                             req.data.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    std::snprintf(rep.message, sizeof(rep.message), "%s", r.message.c_str());
    if (r.status == 0 && !set_bytes(rep.digest, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        std::snprintf(rep.message, sizeof(rep.message), "digest overflow");
    }
    return rep;
}

// Sign(slot, data) → signature. The private key stays in the provider.
SignReply CryptoProvider::handle_call(const SignReq& req,
                                      CryptoProviderState& /*s*/) {
    SignReply rep = system_services_crypto_SignReply_init_zero;
    auto r = provider().sign(req.key_slot, static_cast<int>(req.algo),
                             req.data.bytes, req.data.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    std::snprintf(rep.message, sizeof(rep.message), "%s", r.message.c_str());
    if (r.status == 0 && !set_bytes(rep.signature, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        std::snprintf(rep.message, sizeof(rep.message), "signature overflow");
    }
    return rep;
}

// Verify(slot, data, signature) → valid.
VerifyReply CryptoProvider::handle_call(const VerifyReq& req,
                                        CryptoProviderState& /*s*/) {
    VerifyReply rep = system_services_crypto_VerifyReply_init_zero;
    auto r = provider().verify(req.key_slot, static_cast<int>(req.algo),
                               req.data.bytes, req.data.size,
                               req.signature.bytes, req.signature.size);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    std::snprintf(rep.message, sizeof(rep.message), "%s", r.message.c_str());
    rep.valid = r.valid;
    return rep;
}

// GetCert(slot) → PEM. Serves the cert; the key is never exposed.
CertReply CryptoProvider::handle_call(const CertReq& req,
                                      CryptoProviderState& /*s*/) {
    CertReply rep = system_services_crypto_CertReply_init_zero;
    auto r = provider().get_cert(req.slot);
    rep.status = static_cast<system_services_crypto_CryptoStatus>(r.status);
    std::snprintf(rep.message, sizeof(rep.message), "%s", r.message.c_str());
    if (r.status == 0 && !set_bytes(rep.pem, r.bytes)) {
        rep.status = system_services_crypto_CryptoStatus_CryptoStatus_BACKEND_ERROR;
        std::snprintf(rep.message, sizeof(rep.message), "pem overflow");
    }
    return rep;
}

}  // namespace ara::crypto
