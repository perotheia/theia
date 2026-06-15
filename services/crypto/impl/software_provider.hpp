// software_provider — the OpenSSL-backed Crypto Provider for the crypto FC.
//
// Implements BOTH crypto surfaces (docs/autosar/services/crypto.md):
//
//   * the AUTOSAR CONTEXT lifecycle (create → start → update* → finish) — the
//     faithful ara::crypto shape, holding an in-progress EVP_MD_CTX per handle
//     so arbitrary-length data streams in chunks (no per-message size cap);
//   * the one-shot convenience methods (hash/sign/verify/get_cert) for small
//     payloads (a TLS handshake hash, a cert fetch).
//
// All OpenSSL lives behind this header so the gen_server handler TU stays clean
// (mirrors per isolating etcd in etcd_store.hpp). KeySlot facade (v1): a slot
// "<name>" → <slot_dir>/<name>.key (PEM private) + .crt (PEM cert). The PRIVATE
// KEY NEVER LEAVES this process — sign returns only the signature; there is no
// export op. Real opaque slots / an HSM replace the file map later.

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

// CryptoStatus ordinals (must match the .art enum order).
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

// CtxKind ordinals (must match the .art enum order).
enum CKind : int { CK_HASH = 0, CK_SIGNER = 1, CK_VERIFIER = 2 };

// Result of a provider op: status + (on success) output bytes / valid.
struct ProviderResult {
    uint32_t    status = CS_OK;
    std::string message;
    std::vector<uint8_t> bytes;   // digest / signature / pem / cert
    bool        valid = false;    // verify result
};

class SoftwareProvider {
public:
    explicit SoftwareProvider(std::string slot_dir)
        : slot_dir_(std::move(slot_dir)) {}

    // ====================================================================
    // Context lifecycle — the streaming ara::crypto API
    // ====================================================================

    // create(kind, algo, key_slot) → handle (0 on error). SIGNER/VERIFIER bind
    // the slot now; HASH ignores it. The EVP_MD_CTX is built at Start().
    uint64_t create(int kind, int algo, const std::string& key_slot,
                    ProviderResult& err) {
        if (!md_for_(algo)) { err = mk_(CS_BAD_ALGO, "bad algo"); return 0; }
        if (kind != CK_HASH && kind != CK_SIGNER && kind != CK_VERIFIER) {
            err = mk_(CS_BAD_INPUT, "bad ctx kind"); return 0;
        }
        if (kind == CK_SIGNER && !load_private_(key_slot)) {
            err = mk_(CS_UNKNOWN_IDENTIFIER, "unknown slot / unreadable key");
            return 0;
        }
        if (kind == CK_VERIFIER && !load_public_(key_slot)) {
            err = mk_(CS_UNKNOWN_IDENTIFIER, "unknown slot / unreadable cert");
            return 0;
        }
        std::lock_guard<std::mutex> lk(mu_);
        uint64_t h = ++next_ctx_;
        CtxState st;                    // md defaults to {nullptr, free}
        st.kind = kind; st.algo = algo; st.key_slot = key_slot;
        st.started = false;
        ctxs_.emplace(h, std::move(st));
        return h;
    }

    // start(ctx) — (re)initialize the EVP op. ara::crypto ctx->Start().
    ProviderResult start(uint64_t ctx) {
        std::lock_guard<std::mutex> lk(mu_);
        auto* c = find_(ctx);
        if (!c) return mk_(CS_BAD_CONTEXT, "unknown context");
        c->md.reset(EVP_MD_CTX_new());
        const EVP_MD* md = md_for_(c->algo);
        int ok = 0;
        if (c->kind == CK_HASH) {
            ok = EVP_DigestInit_ex(c->md.get(), md, nullptr);
        } else if (c->kind == CK_SIGNER) {
            ok = EVP_DigestSignInit(c->md.get(), nullptr, md, nullptr,
                                    load_private_(c->key_slot));
        } else {  // VERIFIER
            ok = EVP_DigestVerifyInit(c->md.get(), nullptr, md, nullptr,
                                      load_public_(c->key_slot));
        }
        if (ok != 1) return mk_(CS_BACKEND_ERROR, "ctx init failed");
        c->started = true;
        return mk_(CS_OK, "");
    }

    // update(ctx, chunk) — feed one chunk. Call repeatedly for large data.
    ProviderResult update(uint64_t ctx, const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        auto* c = find_(ctx);
        if (!c) return mk_(CS_BAD_CONTEXT, "unknown context");
        if (!c->started || !c->md) return mk_(CS_BAD_CONTEXT, "ctx not started");
        int ok = (c->kind == CK_HASH)
                   ? EVP_DigestUpdate(c->md.get(), data, len)
                 : (c->kind == CK_SIGNER)
                   ? EVP_DigestSignUpdate(c->md.get(), data, len)
                   : EVP_DigestVerifyUpdate(c->md.get(), data, len);
        if (ok != 1) return mk_(CS_BACKEND_ERROR, "update failed");
        return mk_(CS_OK, "");
    }

    // finish(ctx, sig) — complete + RELEASE the handle. HASH→digest;
    // SIGNER→signature; VERIFIER→valid (sig = the signature to check).
    ProviderResult finish(uint64_t ctx, const uint8_t* sig, size_t siglen) {
        std::lock_guard<std::mutex> lk(mu_);
        auto* c = find_(ctx);
        if (!c) return mk_(CS_BAD_CONTEXT, "unknown context");
        if (!c->started || !c->md) return mk_(CS_BAD_CONTEXT, "ctx not started");
        ProviderResult r;
        if (c->kind == CK_HASH) {
            unsigned int dlen = 0;
            std::vector<uint8_t> dig(EVP_MAX_MD_SIZE);
            if (EVP_DigestFinal_ex(c->md.get(), dig.data(), &dlen) != 1)
                r = mk_(CS_BACKEND_ERROR, "digest final failed");
            else { dig.resize(dlen); r = ok_(std::move(dig)); }
        } else if (c->kind == CK_SIGNER) {
            size_t n = 0;
            if (EVP_DigestSignFinal(c->md.get(), nullptr, &n) != 1)
                r = mk_(CS_BACKEND_ERROR, "sign sizing failed");
            else {
                std::vector<uint8_t> s(n);
                if (EVP_DigestSignFinal(c->md.get(), s.data(), &n) != 1)
                    r = mk_(CS_BACKEND_ERROR, "sign final failed");
                else { s.resize(n); r = ok_(std::move(s)); }
            }
        } else {  // VERIFIER
            int rc = EVP_DigestVerifyFinal(c->md.get(), sig, siglen);
            r.status = CS_OK;
            r.valid  = (rc == 1);
            if (rc < 0) { r.status = CS_BACKEND_ERROR; r.message = "verify error"; }
            else if (rc == 0) { r.status = CS_AUTH_TAG_FAILED; }
        }
        ctxs_.erase(ctx);   // release the handle on Finish
        return r;
    }

    // ====================================================================
    // One-shot convenience (small payloads)
    // ====================================================================

    ProviderResult hash(int algo, const uint8_t* data, size_t len) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return mk_(CS_BAD_ALGO, "bad algo");
        MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        unsigned int dlen = 0;
        std::vector<uint8_t> dig(EVP_MAX_MD_SIZE);
        if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1 ||
            EVP_DigestUpdate(ctx.get(), data, len) != 1 ||
            EVP_DigestFinal_ex(ctx.get(), dig.data(), &dlen) != 1)
            return mk_(CS_BACKEND_ERROR, "digest failed");
        dig.resize(dlen);
        return ok_(std::move(dig));
    }

    ProviderResult sign(const std::string& slot, int algo,
                        const uint8_t* data, size_t len) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return mk_(CS_BAD_ALGO, "bad algo");
        EVP_PKEY* key = load_private_(slot);
        if (!key) return mk_(CS_UNKNOWN_IDENTIFIER, "unknown slot / unreadable key");
        MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, md, nullptr, key) != 1)
            return mk_(CS_BACKEND_ERROR, "sign init failed");
        size_t n = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &n, data, len) != 1)
            return mk_(CS_BACKEND_ERROR, "sign sizing failed");
        std::vector<uint8_t> sig(n);
        if (EVP_DigestSign(ctx.get(), sig.data(), &n, data, len) != 1)
            return mk_(CS_BACKEND_ERROR, "sign failed");
        sig.resize(n);
        return ok_(std::move(sig));
    }

    ProviderResult verify(const std::string& slot, int algo,
                          const uint8_t* data, size_t len,
                          const uint8_t* sig, size_t siglen) {
        const EVP_MD* md = md_for_(algo);
        if (!md) return mk_(CS_BAD_ALGO, "bad algo");
        EVP_PKEY* key = load_public_(slot);
        if (!key) return mk_(CS_UNKNOWN_IDENTIFIER, "unknown slot / unreadable cert");
        MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, key) != 1)
            return mk_(CS_BACKEND_ERROR, "verify init failed");
        int rc = EVP_DigestVerify(ctx.get(), sig, siglen, data, len);
        ProviderResult r;
        r.valid  = (rc == 1);
        r.status = rc < 0 ? CS_BACKEND_ERROR : (rc == 0 ? CS_AUTH_TAG_FAILED : CS_OK);
        if (rc < 0) r.message = "verify error";
        return r;
    }

    ProviderResult get_cert(const std::string& slot) {
        std::string pem = read_file_(cert_path_(slot));
        if (pem.empty()) return mk_(CS_UNKNOWN_IDENTIFIER, "unknown slot / unreadable cert");
        return ok_(std::vector<uint8_t>(pem.begin(), pem.end()));
    }

    // slot_info(slot) → {family, usage, exportable}. v1: derive the family from
    // the key, usage = sign|verify (a cert/key slot), exportable=false (the key
    // file is on disk but the API never returns it). present=false if no key.
    bool slot_info(const std::string& slot, std::string& family,
                   uint32_t& usage, bool& exportable) {
        EVP_PKEY* k = load_private_(slot);
        if (!k) k = load_public_(slot);
        if (!k) return false;
        int bits = EVP_PKEY_bits(k);
        int id = EVP_PKEY_id(k);
        family = (id == EVP_PKEY_RSA ? "RSA-" : id == EVP_PKEY_EC ? "EC-" : "PKEY-")
               + std::to_string(bits);
        usage = 4 | 8;        // USE_SIGN | USE_VERIFY
        exportable = false;   // key never leaves the provider
        return true;
    }

private:
    using MdCtx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    // One in-progress context. md is null until Start().
    struct CtxState {
        int         kind;
        int         algo;
        std::string key_slot;
        MdCtx       md{nullptr, EVP_MD_CTX_free};
        bool        started;
    };

    static const EVP_MD* md_for_(int algo) {
        switch (algo) { case 0: return EVP_sha256();
                        case 1: return EVP_sha384();
                        case 2: return EVP_sha512();
                        default: return nullptr; }
    }
    std::string key_path_(const std::string& s) const { return slot_dir_+"/"+s+".key"; }
    std::string cert_path_(const std::string& s) const { return slot_dir_+"/"+s+".crt"; }

    CtxState* find_(uint64_t h) {           // caller holds mu_
        auto it = ctxs_.find(h);
        return it == ctxs_.end() ? nullptr : &it->second;
    }

    EVP_PKEY* load_private_(const std::string& slot) {
        std::lock_guard<std::mutex> lk(keymu_);
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
    EVP_PKEY* load_public_(const std::string& slot) {
        std::lock_guard<std::mutex> lk(keymu_);
        auto it = pub_.find(slot);
        if (it != pub_.end()) return it->second.get();
        FILE* f = ::fopen(cert_path_(slot).c_str(), "r");
        if (!f) return nullptr;
        std::unique_ptr<X509, decltype(&X509_free)>
            cert(PEM_read_X509(f, nullptr, nullptr, nullptr), X509_free);
        ::fclose(f);
        if (!cert) return nullptr;
        EVP_PKEY* pk = X509_get_pubkey(cert.get());
        if (!pk) return nullptr;
        auto* raw = pk;
        pub_.emplace(slot, PkeyPtr(pk, EVP_PKEY_free));
        return raw;
    }

    static std::string read_file_(const std::string& path) {
        FILE* f = ::fopen(path.c_str(), "rb");
        if (!f) return "";
        std::string out; char buf[4096]; size_t n;
        while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        ::fclose(f);
        return out;
    }

    static ProviderResult ok_(std::vector<uint8_t> b) {
        ProviderResult r; r.status = CS_OK; r.bytes = std::move(b); return r;
    }
    static ProviderResult mk_(uint32_t st, std::string msg) {
        ProviderResult r; r.status = st; r.message = std::move(msg); return r;
    }

    std::string slot_dir_;
    std::mutex  mu_;          // guards ctxs_ + next_ctx_
    std::mutex  keymu_;       // guards the key caches
    uint64_t    next_ctx_ = 0;
    std::map<uint64_t, CtxState> ctxs_;
    std::map<std::string, PkeyPtr> priv_;
    std::map<std::string, PkeyPtr> pub_;
};

}  // namespace ara::crypto
