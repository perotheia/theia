// hardware_provider — a STUB Crypto Provider standing in for an HSM / TPM /
// Secure Element backend (the AUTOSAR Hardware Crypto Provider).
//
// Phase 4 of docs/tasks/PROGRESS/grpc-certificates.md. It implements the SAME
// ICryptoProvider surface as the SoftwareProvider, but has no real hardware: the
// keyed ops (sign / verify / priv_op / context create-with-key) return
// CS_BACKEND_ERROR with a clear "HSM not present" message. This PROVES the seam
// — the manager can route a slot to this provider with zero change to the FC
// handler or any caller (com TLS). A real driver (PKCS#11 / TA / NVMe-HSM) drops
// in here behind the same methods.
//
// Hash works (no key), to show that non-keyed ops are provider-agnostic.

#pragma once

#include <openssl/evp.h>

#include "impl/crypto_provider.hpp"
#include "impl/software_provider.hpp"   // reuse OpenSSL hash (no key involved)

namespace ara::crypto {

class HardwareProvider : public ICryptoProvider {
public:
    // The stub is configured with the HSM "device" id (a path / PKCS#11 URI in a
    // real impl). Unused today; logged so a deploy sees which backend a slot
    // resolved to.
    explicit HardwareProvider(std::string device)
        : device_(std::move(device)), sw_("") {}

    const char* name() const override { return "HSM stub"; }

    // ---- keyed ops: not available without real hardware -------------------
    uint64_t create(int kind, int /*algo*/, const std::string& /*slot*/,
                    ProviderResult& err) override {
        if (kind == CK_HASH) {
            // A hash context needs no key — but our stub doesn't keep contexts.
            // Route hash through the software path's one-shot instead (callers
            // that want streaming hash use the software provider). Signal that a
            // keyed/streaming ctx on the HSM stub isn't available.
        }
        err = not_present_("create");
        return 0;
    }
    ProviderResult start(uint64_t)                 override { return not_present_("start"); }
    ProviderResult update(uint64_t, const uint8_t*, size_t) override { return not_present_("update"); }
    ProviderResult finish(uint64_t, const uint8_t*, size_t) override { return not_present_("finish"); }

    ProviderResult sign(const std::string&, int, const uint8_t*, size_t) override {
        return not_present_("sign");
    }
    ProviderResult verify(const std::string&, int, const uint8_t*, size_t,
                          const uint8_t*, size_t) override {
        return not_present_("verify");
    }
    ProviderResult priv_op(const std::string&, const uint8_t*, size_t) override {
        return not_present_("priv_op");
    }
    ProviderResult get_cert(const std::string&) override {
        return not_present_("get_cert");
    }

    // ---- non-keyed: hash is backend-agnostic, served via OpenSSL ----------
    ProviderResult hash(int algo, const uint8_t* data, size_t len) override {
        return sw_.hash(algo, data, len);
    }

    // The stub holds no slots.
    bool slot_info(const std::string&, std::string&, uint32_t&, bool&) override {
        return false;
    }

private:
    ProviderResult not_present_(const char* op) {
        ProviderResult r;
        r.status  = CS_BACKEND_ERROR;
        r.message = std::string("HSM stub (device='") + device_ + "'): " + op +
                    " not implemented — no hardware backend";
        return r;
    }

    std::string      device_;
    SoftwareProvider sw_;   // for backend-agnostic hash only
};

}  // namespace ara::crypto
