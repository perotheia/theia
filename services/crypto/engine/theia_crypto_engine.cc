// theia_crypto_engine — an OpenSSL ENGINE that proxies RSA private-key
// operations to the Theia crypto FC (services/crypto) over TIPC, so a TLS
// server's PRIVATE KEY NEVER ENTERS the calling process.
//
// Phase 3 of docs/tasks/PROGRESS/grpc-certificates.md. grpc's TLS layer
// (system OpenSSL, which keeps ENGINE support — src/core/tsi/
// ssl_transport_security.cc, ssl_ctx_use_engine_private_key) loads a key of the
// form `engine:theia_crypto:<slot>`. We return an EVP_PKEY whose:
//   * PUBLIC part is the cert's RSA public key (read from <slot>.crt — public,
//     so reading it here is fine), and
//   * PRIVATE RSA op (RSA_meth sign / priv_enc) is REDIRECTED to the crypto FC's
//     PrivateKeyOp(slot, input) -> input^d mod n over TIPC. The key bytes stay
//     in the FC; only the raw RSA result comes back.
//
// Built as a standalone .so (no Theia runtime dep) — it does a raw AF_TIPC
// SEQPACKET call, encoding the request with the crypto FC's nanopb types and
// the runtime's TheiaMsgHeader. Load via OpenSSL's dynamic engine loader.

#include <openssl/engine.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/err.h>

#include <pb_encode.h>
#include <pb_decode.h>
#include "system/services/crypto/crypto.pb.h"

#include "TheiaMsgHeader.hh"    // runtime wire header (24B)
#include "RemoteCodec.hh"       // hash_msg_type_ (service_id)

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kEngineId   = "theia_crypto";
constexpr const char* kEngineName = "Theia crypto FC RSA proxy engine";

// services/crypto CryptoProvider node (the oneshot port serves PrivateKeyOp).
constexpr uint32_t kCryptoType     = 0x80010006u;
constexpr uint32_t kCryptoInstance = 0u;

// Per-key app-data: the slot name (the FC reference). Stored on the RSA via an
// ex-data index so the sign callback knows which slot to ask the FC for.
int g_rsa_slot_idx = -1;

// ---- the raw TIPC PrivateKeyOp call --------------------------------------
//
// Connect a SEQPACKET to the crypto node, send a GEN_CALL framed
// [TheiaMsgHeader][PrivOpReq nanopb], read [TheiaMsgHeader][PrivOpReply nanopb].
// Returns the output bytes (input^d mod n) or empty on failure.
std::vector<uint8_t> fc_priv_op(const std::string& slot,
                                const uint8_t* input, size_t inlen) {
    std::vector<uint8_t> out;
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) { std::perror("[theia_engine] socket"); return out; }
    struct sockaddr_tipc addr{};
    addr.family   = AF_TIPC;
    addr.addrtype = TIPC_SERVICE_ADDR;
    addr.addr.name.name.type     = kCryptoType;
    addr.addr.name.name.instance = kCryptoInstance;
    addr.addr.name.domain = 0;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[theia_engine] connect crypto 0x%08x failed\n",
                     kCryptoType);
        ::close(fd); return out;
    }

    // Encode PrivOpReq.
    system_services_crypto_PrivOpReq req =
        system_services_crypto_PrivOpReq_init_zero;
    std::snprintf(req.slot, sizeof(req.slot), "%s", slot.c_str());
    if (inlen > sizeof(req.input.bytes)) { ::close(fd); return out; }
    std::memcpy(req.input.bytes, input, inlen);
    req.input.size = static_cast<pb_size_t>(inlen);

    uint8_t reqbuf[1024];
    pb_ostream_t os = pb_ostream_from_buffer(reqbuf, sizeof(reqbuf));
    if (!pb_encode(&os, system_services_crypto_PrivOpReq_fields, &req)) {
        ::close(fd); return out;
    }

    ::theia::runtime::TheiaMsgHeader hdr{};
    hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
    hdr.msg_type           = ::theia::runtime::kMsgGenCall;
    hdr.proto_len          = static_cast<uint16_t>(os.bytes_written);
    hdr.rpc.service_id     =
        ::theia::runtime::hash_msg_type_("system_services_crypto_PrivOpReq");
    hdr.rpc.method_id      = 0;
    hdr.rpc.correlation_id = 1;

    std::vector<uint8_t> frame(sizeof(hdr) + os.bytes_written);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), reqbuf, os.bytes_written);
    if (::send(fd, frame.data(), frame.size(), 0) < 0) { ::close(fd); return out; }

    // Read the reply.
    std::vector<uint8_t> rbuf(4096);
    ssize_t n = ::recv(fd, rbuf.data(), rbuf.size(), 0);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(::theia::runtime::TheiaMsgHeader)))
        return out;
    ::theia::runtime::TheiaMsgHeader rh{};
    std::memcpy(&rh, rbuf.data(), sizeof(rh));
    const uint8_t* body = rbuf.data() + sizeof(rh);
    size_t avail = static_cast<size_t>(n) - sizeof(rh);
    size_t plen  = rh.proto_len <= avail ? rh.proto_len : avail;

    system_services_crypto_PrivOpReply rep =
        system_services_crypto_PrivOpReply_init_zero;
    pb_istream_t is = pb_istream_from_buffer(body, plen);
    if (!pb_decode(&is, system_services_crypto_PrivOpReply_fields, &rep))
        return out;
    if (rep.status != system_services_crypto_CryptoStatus_CryptoStatus_OK) {
        std::fprintf(stderr, "[theia_engine] PrivateKeyOp status=%d %s\n",
                     (int)rep.status, rep.message);
        return out;
    }
    out.assign(rep.output.bytes, rep.output.bytes + rep.output.size);
    return out;
}

// ---- RSA_METHOD: redirect the private op to the FC ------------------------
//
// The legacy RSA_meth contract: priv_enc gets the UNPADDED input + a `padding`
// mode and must (1) apply the padding to a modulus-sized block, then (2) do the
// raw modexp `block^d mod n`. Padding is PUBLIC + deterministic (no key), so we
// do it here; only the keyed modexp goes to the FC (which never sees the key).
int theia_rsa_priv_enc(int flen, const unsigned char* from, unsigned char* to,
                       RSA* rsa, int padding) {
    const char* slot = static_cast<const char*>(RSA_get_ex_data(rsa, g_rsa_slot_idx));
    if (!slot) return -1;
    const int k = RSA_size(rsa);
    std::vector<unsigned char> block(k);
    int ok = 0;
    switch (padding) {
        case RSA_PKCS1_PADDING:          // TLS 1.2 RSA signatures
            ok = RSA_padding_add_PKCS1_type_1(block.data(), k, from, flen);
            break;
        case RSA_X931_PADDING:
            ok = RSA_padding_add_X931(block.data(), k, from, flen);
            break;
        case RSA_NO_PADDING:             // caller already padded (e.g. PSS)
            if (flen != k) return -1;
            std::memcpy(block.data(), from, k);
            ok = 1;
            break;
        default:
            std::fprintf(stderr, "[theia_engine] unsupported padding %d\n", padding);
            return -1;
    }
    if (ok <= 0) return -1;
    // Now the keyed primitive: block^d mod n — done IN the FC.
    auto out = fc_priv_op(slot, block.data(), block.size());
    if (out.empty()) return -1;
    std::memcpy(to, out.data(), out.size());
    return static_cast<int>(out.size());
}

// priv_dec (RSA decrypt) — the raw keyed primitive on an already-modulus-sized
// ciphertext; the caller strips padding. TLS RSA-KEX (legacy) uses this.
int theia_rsa_priv_dec(int flen, const unsigned char* from, unsigned char* to,
                       RSA* rsa, int padding) {
    const char* slot = static_cast<const char*>(RSA_get_ex_data(rsa, g_rsa_slot_idx));
    if (!slot) return -1;
    const int k = RSA_size(rsa);
    if (flen != k) return -1;
    auto raw = fc_priv_op(slot, from, static_cast<size_t>(flen));
    if (raw.empty()) return -1;
    // Strip the padding the caller asked for (PUBLIC; no key).
    int n = -1;
    switch (padding) {
        case RSA_PKCS1_PADDING:
            n = RSA_padding_check_PKCS1_type_2(to, k, raw.data() + 1,
                                               k - 1, k);
            break;
        case RSA_NO_PADDING:
            std::memcpy(to, raw.data(), k); n = k; break;
        default: return -1;
    }
    return n;
}

RSA_METHOD* g_rsa_method = nullptr;

const RSA_METHOD* theia_rsa_method() {
    if (g_rsa_method) return g_rsa_method;
    g_rsa_method = RSA_meth_dup(RSA_PKCS1_OpenSSL());
    RSA_meth_set1_name(g_rsa_method, "Theia crypto FC RSA");
    RSA_meth_set_priv_enc(g_rsa_method, theia_rsa_priv_enc);
    RSA_meth_set_priv_dec(g_rsa_method, theia_rsa_priv_dec);
    return g_rsa_method;
}

// ---- ENGINE_load_privkey: build the proxy EVP_PKEY ------------------------
//
// key_id is the SLOT name. We read the slot's CERT (public, on disk) for the
// public key + parameters, then swap in our RSA_METHOD so the private op goes
// to the FC. The private key file is NEVER read.
EVP_PKEY* theia_load_privkey(ENGINE* /*e*/, const char* key_id,
                             UI_METHOD* /*ui*/, void* /*cb_data*/) {
    if (!key_id) return nullptr;
    const char* dir = ::getenv("THEIA_CRYPTO_SLOT_DIR");
    std::string cert_path = std::string(dir ? dir : "/etc/theia/crypto")
                          + "/" + key_id + ".crt";
    FILE* f = ::fopen(cert_path.c_str(), "r");
    if (!f) {
        std::fprintf(stderr, "[theia_engine] no cert for slot '%s' (%s)\n",
                     key_id, cert_path.c_str());
        return nullptr;
    }
    X509* cert = PEM_read_X509(f, nullptr, nullptr, nullptr);
    ::fclose(f);
    if (!cert) return nullptr;
    EVP_PKEY* pub = X509_get_pubkey(cert);   // the cert's public key
    X509_free(cert);
    if (!pub || EVP_PKEY_base_id(pub) != EVP_PKEY_RSA) {
        if (pub) EVP_PKEY_free(pub);
        std::fprintf(stderr, "[theia_engine] slot '%s' not RSA\n", key_id);
        return nullptr;
    }
    // Clone the public RSA, attach our method + the slot name, wrap in EVP_PKEY.
    const RSA* pub_rsa = EVP_PKEY_get0_RSA(pub);
    RSA* rsa = RSAPublicKey_dup(const_cast<RSA*>(pub_rsa));
    EVP_PKEY_free(pub);
    if (!rsa) return nullptr;
    RSA_set_method(rsa, theia_rsa_method());
    // Stash the slot name on the RSA so priv_enc knows which slot to ask for.
    char* slot_copy = OPENSSL_strdup(key_id);
    RSA_set_ex_data(rsa, g_rsa_slot_idx, slot_copy);
    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(pkey, rsa);          // pkey now owns rsa
    std::fprintf(stderr, "[theia_engine] loaded proxy key for slot '%s' "
                 "(private op → crypto FC 0x%08x)\n", key_id, kCryptoType);
    return pkey;
}

int theia_engine_init(ENGINE* /*e*/) { return 1; }

int theia_engine_bind(ENGINE* e, const char* id) {
    if (id && std::strcmp(id, kEngineId) != 0) return 0;
    if (g_rsa_slot_idx < 0)
        g_rsa_slot_idx = RSA_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    if (!ENGINE_set_id(e, kEngineId) ||
        !ENGINE_set_name(e, kEngineName) ||
        !ENGINE_set_init_function(e, theia_engine_init) ||
        !ENGINE_set_RSA(e, theia_rsa_method()) ||
        !ENGINE_set_load_privkey_function(e, theia_load_privkey)) {
        return 0;
    }
    return 1;
}

}  // namespace

extern "C" {
IMPLEMENT_DYNAMIC_CHECK_FN()
IMPLEMENT_DYNAMIC_BIND_FN(theia_engine_bind)
}
