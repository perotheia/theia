// software_provider — the OpenSSL-backed Crypto Provider for the crypto FC.
//
// Phase 2 of docs/tasks/PROGRESS/grpc-certificates.md. The SoftwareProvider is
// the only provider today (a HardwareProvider/HSM stub comes in phase 4 behind
// the SAME surface). It implements the crypto primitives over OpenSSL EVP:
//
//   hash(algo, data)              → digest          (pure transform, no key)
//   sign(slot, algo, data)        → signature       (private key by slot)
//   verify(slot, algo, data, sig) → valid           (public key / cert by slot)
//   get_cert(slot)                → PEM              (cert, never the key)
//
// KeySlot facade (v1): a slot "<name>" maps to <slot_dir>/<name>.key (PEM
// private key) + <slot_dir>/<name>.crt (PEM cert). Loaded lazily, cached. The
// PRIVATE KEY NEVER LEAVES this process — sign() returns only the signature, and
// there is no "export key" op. Real opaque slots / an HSM replace the file map
// later without changing the caller (the ara::crypto portability point).
//
// All OpenSSL lives behind this header so the gen_server handler TU stays clean.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/err.h>

namespace ara::crypto {

// Result of a provider op: ok + (on success) the output bytes, else a status
// ordinal matching the .art CryptoStatus enum + a short message.
struct ProviderResult {
    uint32_t    status = 0;   // 0=OK, else CryptoStatus ordinal
    std::string message;
    std::vector<uint8_t> bytes;   // digest / signature / pem (op-dependent)
    bool        valid = false;    // verify() result
};

class SoftwareProvider {
public:
    explicit SoftwareProvider(std::string slot_dir)
        : slot_dir_(std::move(slot_dir)) {}

    // ---- hash(algo, data) → digest (no key) -------------------------------
    ProviderResult hash(int algo, const uint8_t* data, size_t len) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return err_(2, "bad algo");        // BAD_ALGO
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
            ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        unsigned int dlen = 0;
        std::vector<uint8_t> dig(EVP_MAX_MD_SIZE);
        if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1 ||
            EVP_DigestUpdate(ctx.get(), data, len) != 1 ||
            EVP_DigestFinal_ex(ctx.get(), dig.data(), &dlen) != 1) {
            return err_(3, "digest failed");         // BACKEND_ERROR
        }
        dig.resize(dlen);
        return ok_(std::move(dig));
    }

    // ---- sign(slot, algo, data) → signature (digest-then-sign) ------------
    ProviderResult sign(const std::string& slot, int algo,
                        const uint8_t* data, size_t len) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return err_(2, "bad algo");
        EVP_PKEY* key = load_private_(slot);
        if (!key) return err_(1, "unknown slot or unreadable key");  // UNKNOWN_SLOT
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
            ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, md, nullptr, key) != 1)
            return err_(3, "sign init failed");
        size_t siglen = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &siglen, data, len) != 1)
            return err_(3, "sign sizing failed");
        std::vector<uint8_t> sig(siglen);
        if (EVP_DigestSign(ctx.get(), sig.data(), &siglen, data, len) != 1)
            return err_(3, "sign failed");
        sig.resize(siglen);
        return ok_(std::move(sig));
    }

    // ---- verify(slot, algo, data, sig) → valid (public key / cert) --------
    ProviderResult verify(const std::string& slot, int algo,
                          const uint8_t* data, size_t len,
                          const uint8_t* sig, size_t siglen) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return err_(2, "bad algo");
        EVP_PKEY* key = load_public_(slot);
        if (!key) return err_(1, "unknown slot or unreadable cert");
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
            ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, key) != 1)
            return err_(3, "verify init failed");
        int rc = EVP_DigestVerify(ctx.get(), sig, siglen, data, len);
        ProviderResult r;
        r.status = 0;
        r.valid  = (rc == 1);   // 1=valid, 0=invalid sig, <0=error
        if (rc < 0) { r.status = 3; r.message = "verify error"; }
        return r;
    }

    // ---- get_cert(slot) → PEM (cert only; the key never leaves) -----------
    ProviderResult get_cert(const std::string& slot) {
        std::string pem = read_file_(cert_path_(slot));
        if (pem.empty()) return err_(1, "unknown slot or unreadable cert");
        return ok_(std::vector<uint8_t>(pem.begin(), pem.end()));
    }

private:
    static const EVP_MD* md_for_(int algo) {
        switch (algo) {                  // HashAlgo ordinals (0/1/2)
            case 0: return EVP_sha256();
            case 1: return EVP_sha384();
            case 2: return EVP_sha512();
            default: return nullptr;
        }
    }
    std::string key_path_(const std::string& slot) const {
        return slot_dir_ + "/" + slot + ".key";
    }
    std::string cert_path_(const std::string& slot) const {
        return slot_dir_ + "/" + slot + ".crt";
    }

    // Lazy-load + cache the private key for a slot (PEM). Owned by this
    // process for its lifetime; never serialized out.
    EVP_PKEY* load_private_(const std::string& slot) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = priv_.find(slot);
        if (it != priv_.end()) return it->second.get();
        FILE* f = ::fopen(key_path_(slot).c_str(), "r");
        if (!f) return nullptr;
        EVP_PKEY* k = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
        ::fclose(f);
        if (!k) return nullptr;
        auto* raw = k;
        priv_.emplace(slot, PkeyPtr(k, EVP_PKEY_free));
        return raw;
    }

    // Lazy-load + cache the PUBLIC key (from the slot's cert) for verify.
    EVP_PKEY* load_public_(const std::string& slot) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pub_.find(slot);
        if (it != pub_.end()) return it->second.get();
        FILE* f = ::fopen(cert_path_(slot).c_str(), "r");
        if (!f) return nullptr;
        std::unique_ptr<X509, decltype(&X509_free)>
            cert(PEM_read_X509(f, nullptr, nullptr, nullptr), X509_free);
        ::fclose(f);
        if (!cert) return nullptr;
        EVP_PKEY* pk = X509_get_pubkey(cert.get());   // owned ref
        if (!pk) return nullptr;
        auto* raw = pk;
        pub_.emplace(slot, PkeyPtr(pk, EVP_PKEY_free));
        return raw;
    }

    static std::string read_file_(const std::string& path) {
        FILE* f = ::fopen(path.c_str(), "rb");
        if (!f) return "";
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        ::fclose(f);
        return out;
    }

    static ProviderResult ok_(std::vector<uint8_t> bytes) {
        ProviderResult r; r.status = 0; r.bytes = std::move(bytes); return r;
    }
    static ProviderResult err_(uint32_t status, std::string msg) {
        ProviderResult r; r.status = status; r.message = std::move(msg);
        return r;
    }

    using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    std::string slot_dir_;
    std::mutex  mu_;
    std::map<std::string, PkeyPtr> priv_;
    std::map<std::string, PkeyPtr> pub_;
};

}  // namespace ara::crypto
