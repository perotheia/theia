// crypto_provider — the abstract Crypto Provider interface (AUTOSAR CP pillar).
//
// The Crypto Service Manager (provider_manager.hpp) routes each request to a
// provider by SLOT; callers never know whether OpenSSL (SoftwareProvider) or an
// HSM (HardwareProvider) serves it. Swapping the backend for a slot is a config
// change inside the FC — the AUTOSAR portability point. Phase 4 of
// docs/tasks/PROGRESS/grpc-certificates.md.
//
// The shared result type + status/kind enums live here so both providers and
// the manager use one vocabulary.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ara::crypto {

// CryptoStatus ordinals (must match the .art CryptoStatus enum order).
enum CStatus : uint32_t {
    CS_OK = 0,
    CS_UNKNOWN_IDENTIFIER = 1,
    CS_INCOMPATIBLE_ARGS  = 2,
    CS_USAGE_VIOLATION    = 3,
    CS_AUTH_TAG_FAILED    = 4,
    CS_BAD_ALGO           = 5,
    CS_BAD_CONTEXT        = 6,
    CS_BACKEND_ERROR      = 7,
    CS_BAD_INPUT          = 8,
};

// CtxKind ordinals (must match the .art CtxKind enum order).
enum CKind : int { CK_HASH = 0, CK_SIGNER = 1, CK_VERIFIER = 2 };

// Result of a provider op: status + (on success) output bytes / valid.
struct ProviderResult {
    uint32_t    status = CS_OK;
    std::string message;
    std::vector<uint8_t> bytes;   // digest / signature / pem / cert
    bool        valid = false;    // verify result
};

// The Crypto Provider surface — every op the manager may dispatch. A backend
// (Software=OpenSSL, Hardware=HSM/TPM) implements ALL of these; a backend that
// can't do an op returns CS_BACKEND_ERROR with a clear message (the HSM stub).
//
// `ctx` lifecycle handles are provider-LOCAL (the manager routes a ctx op to
// the same provider that created it — see provider_manager). Slots that resolve
// to this provider are the only ones it's asked about.
class ICryptoProvider {
public:
    virtual ~ICryptoProvider() = default;

    // Context lifecycle (streaming).
    virtual uint64_t create(int kind, int algo, const std::string& key_slot,
                            ProviderResult& err) = 0;
    virtual ProviderResult start(uint64_t ctx) = 0;
    virtual ProviderResult update(uint64_t ctx, const uint8_t* data, size_t len) = 0;
    virtual ProviderResult finish(uint64_t ctx, const uint8_t* sig, size_t siglen) = 0;

    // One-shot.
    virtual ProviderResult hash(int algo, const uint8_t* data, size_t len) = 0;
    virtual ProviderResult sign(const std::string& slot, int algo,
                                const uint8_t* data, size_t len) = 0;
    virtual ProviderResult verify(const std::string& slot, int algo,
                                  const uint8_t* data, size_t len,
                                  const uint8_t* sig, size_t siglen) = 0;
    virtual ProviderResult get_cert(const std::string& slot) = 0;
    virtual ProviderResult priv_op(const std::string& slot,
                                   const uint8_t* input, size_t len) = 0;

    // Slot introspection. Returns false if this provider doesn't hold the slot.
    virtual bool slot_info(const std::string& slot, std::string& family,
                           uint32_t& usage, bool& exportable) = 0;

    // Drop cached key material (cert rotation: a new cert/key was provisioned
    // to the slot; the next op re-reads it). A backend with no cache is a no-op.
    virtual void reload() = 0;

    // Re-point the slot directory (RESTART_REQUIRED in practice, but the
    // SoftwareProvider can honor it live by also dropping its cache).
    virtual void set_slot_dir(const std::string& dir) = 0;

    // A short label for logs ("OpenSSL software", "HSM stub").
    virtual const char* name() const = 0;
};

}  // namespace ara::crypto
