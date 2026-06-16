// com_tls — shared gRPC server-credentials builder for com's three gRPC
// servers (ComGrpcProxy :7700, TraceForwarder :7710, LogForwarder :7711).
//
// TLS is OPT-IN: when a server cert path is configured (THEIA_COM_TLS_CERT, or
// the `tls_cert` com param), all com endpoints serve over TLS; otherwise they
// stay insecure (the local-dev default — no flag day). All three ports flip
// together because they share this one helper.
//
// Phase 1 of the crypto/TLS plan (docs/tasks/BACKLOG/grpc-certificates.md):
// file-provided PEMs, key in com's memory. Client cert is OPTIONAL
// (REQUEST_BUT_DONT_VERIFY) — encryption always, mTLS identity when a client
// presents a cert, cert-less clients (rtdb/GUI on a dev box) still connect.
// Phase 3 moves the private key behind the crypto FC so it never lives here.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include <openssl/engine.h>   // engine-mode TLS key (phase 3)

#include <pb_encode.h>
#include <pb_decode.h>

#include "TheiaMsgHeader.hh"   // wire header + kBusTypeRpc / kMsgGenCall
#include "RemoteCodec.hh"      // hash_msg_type_ (service_id)
#include "system/services/crypto/crypto.pb.h"   // CertReq / CertReply / CryptoStatus

namespace services_com {

// ── crypto cert agreement ────────────────────────────────────────────────
//
// When a TLS SLOT is configured, com does NOT guess about the certificate from
// disk. It ASKS crypto (the authority) via GetCert(slot) over TIPC and acts on
// the answer:
//   - CERT_OK        — crypto returned OK + a PEM → serve TLS with it.
//   - NO_CERT        — crypto says the slot has no cert (UNKNOWN_IDENTIFIER, or
//                      OK with an empty PEM) → the agreed "no certificate
//                      deployed" case → com runs INSECURE.
//   - UNREACHABLE    — crypto didn't answer / errored (connect/recv fail, or a
//                      non-absence CryptoStatus) → com must NOT silently open an
//                      insecure port. The caller fails CLOSED.
// This is the com/crypto certificate agreement: insecure ONLY on crypto's
// explicit "no cert", never on a crypto outage.
enum class CryptoCert { CERT_OK, NO_CERT, UNREACHABLE };

inline CryptoCert crypto_get_cert(const char* who, const std::string& slot,
                                  std::string& pem_out) {
    constexpr uint32_t kCryptoType = 0x80010006u;   // services/crypto CryptoProvider

    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) return CryptoCert::UNREACHABLE;
    struct sockaddr_tipc addr{};
    addr.family   = AF_TIPC;
    addr.addrtype = TIPC_SERVICE_ADDR;
    addr.addr.name.name.type     = kCryptoType;
    addr.addr.name.name.instance = 0;
    addr.addr.name.domain = 0;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[%s] gRPC: crypto FC unreachable (connect "
                     "0x%08x) — cannot confirm cert\n", who, kCryptoType);
        ::close(fd);
        return CryptoCert::UNREACHABLE;
    }

    // Encode CertReq{slot}.
    system_services_crypto_CertReq req = system_services_crypto_CertReq_init_zero;
    std::snprintf(req.slot, sizeof(req.slot), "%s", slot.c_str());
    uint8_t reqbuf[256];
    pb_ostream_t os = pb_ostream_from_buffer(reqbuf, sizeof(reqbuf));
    if (!pb_encode(&os, system_services_crypto_CertReq_fields, &req)) {
        ::close(fd); return CryptoCert::UNREACHABLE;
    }
    ::theia::runtime::TheiaMsgHeader hdr{};
    hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
    hdr.msg_type           = ::theia::runtime::kMsgGenCall;
    hdr.proto_len          = static_cast<uint16_t>(os.bytes_written);
    hdr.rpc.service_id     =
        ::theia::runtime::hash_msg_type_("system_services_crypto_CertReq");
    hdr.rpc.method_id      = 0;
    hdr.rpc.correlation_id = 1;
    std::string frame(sizeof(hdr) + os.bytes_written, '\0');
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), reqbuf, os.bytes_written);
    if (::send(fd, frame.data(), frame.size(), 0) < 0) {
        ::close(fd); return CryptoCert::UNREACHABLE;
    }

    // Read [TheiaMsgHeader][CertReply nanopb]. CertReply.pem is up to 8 KB.
    std::string rbuf(12 * 1024, '\0');
    ssize_t n = ::recv(fd, rbuf.data(), rbuf.size(), 0);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(::theia::runtime::TheiaMsgHeader)))
        return CryptoCert::UNREACHABLE;
    ::theia::runtime::TheiaMsgHeader rh{};
    std::memcpy(&rh, rbuf.data(), sizeof(rh));
    const uint8_t* body =
        reinterpret_cast<const uint8_t*>(rbuf.data()) + sizeof(rh);
    size_t avail = static_cast<size_t>(n) - sizeof(rh);
    size_t plen  = rh.proto_len <= avail ? rh.proto_len : avail;

    system_services_crypto_CertReply rep =
        system_services_crypto_CertReply_init_zero;
    pb_istream_t is = pb_istream_from_buffer(body, plen);
    if (!pb_decode(&is, system_services_crypto_CertReply_fields, &rep))
        return CryptoCert::UNREACHABLE;

    using CS = system_services_crypto_CryptoStatus;
    // The AGREED "no certificate deployed": crypto explicitly says the slot is
    // unknown, OR answers OK with an empty PEM. Either is an authoritative
    // "there is no cert here" — safe to run insecure.
    if (rep.status == CS::system_services_crypto_CryptoStatus_CryptoStatus_UNKNOWN_IDENTIFIER
        || rep.pem.size == 0) {
        return CryptoCert::NO_CERT;
    }
    if (rep.status == CS::system_services_crypto_CryptoStatus_CryptoStatus_OK) {
        pem_out.assign(reinterpret_cast<const char*>(rep.pem.bytes),
                       rep.pem.size);
        return CryptoCert::CERT_OK;
    }
    // Any other status (BACKEND_ERROR, USAGE_VIOLATION, …) is NOT "no cert" —
    // crypto is present but something is wrong. Do not downgrade silently.
    std::fprintf(stderr, "[%s] gRPC: crypto GetCert(%s) status=%d (%s) — not a "
                 "clean 'no cert'; failing closed\n", who, slot.c_str(),
                 (int)rep.status, rep.message);
    return CryptoCert::UNREACHABLE;
}

// Resolve a TLS material path: prefer the env var, else the com param, else "".
// envvar e.g. "THEIA_COM_TLS_CERT"; param e.g. "tls_cert".
inline std::string tls_path_(const char* envvar, const std::string& param_val) {
    if (const char* e = std::getenv(envvar); e && *e) return e;
    return param_val;
}

inline std::string read_file_(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ENGINE MODE (phase 3 — key never enters com). When THEIA_COM_TLS_SLOT is set,
// com hands gRPC's TLS layer the OpenSSL engine key string
// `engine:theia_crypto:<slot>` instead of the private-key bytes. grpc loads it
// via ENGINE_load_private_key (system OpenSSL keeps engine support), and every
// handshake-signing op is proxied by the theia_crypto engine to the crypto FC's
// PrivateKeyOp over TIPC — the private key NEVER LEAVES the FC. The cert is
// public, so it's still read from the slot's .crt directly.
inline bool engine_mode_key(const char* who, const std::string& slot,
                            std::string& key_out, std::string& cert_out) {
    // Load + register the engine ONCE, thread-safely. com runs three gRPC
    // servers on three runnable threads (ComGrpcProxy/TraceForwarder/
    // LogForwarder); they all call make_server_creds near-simultaneously, and
    // OpenSSL's ENGINE_load_dynamic/ENGINE_add global state is NOT thread-safe.
    // std::call_once serializes it so exactly one thread loads the engine and
    // the rest see the result (no concurrent ENGINE-global races, which made
    // some servers spuriously fail "not loadable").
    static std::once_flag once;
    static bool engine_ok = false;
    std::call_once(once, [who]() {
        const char* so = std::getenv("THEIA_CRYPTO_ENGINE");
        std::string path = so && *so ? so : "theia_crypto_engine.so";
        ENGINE_load_dynamic();
        ENGINE* dyn = ENGINE_by_id("dynamic");
        if (!dyn) {
            std::fprintf(stderr, "[%s] gRPC: no OpenSSL dynamic engine loader\n",
                         who);
            return;
        }
        // After LOAD the `dyn` handle BECOMES the theia_crypto engine (the
        // dynamic loader rebinds it in place). ENGINE_add registers it under id
        // "theia_crypto" so grpc's ENGINE_by_id finds it; ENGINE_init activates.
        ENGINE_ctrl_cmd_string(dyn, "SO_PATH", path.c_str(), 0);
        ENGINE_ctrl_cmd_string(dyn, "ID", "theia_crypto", 0);
        if (!ENGINE_ctrl_cmd_string(dyn, "LOAD", nullptr, 0) ||
            !ENGINE_init(dyn)) {
            std::fprintf(stderr, "[%s] gRPC: theia_crypto engine '%s' not "
                         "loadable — cannot use TLS slot\n", who, path.c_str());
            ENGINE_free(dyn);
            return;
        }
        ENGINE_add(dyn);          // register under id "theia_crypto"
        ENGINE_free(dyn);         // ENGINE_add took a ref
        engine_ok = true;
    });
    if (!engine_ok) return false;
    key_out = "engine:theia_crypto:" + slot;
    // The cert is NO LONGER read here — it comes from crypto's GetCert reply
    // (the certificate agreement in make_server_creds). engine_mode_key only
    // sets up the engine + the opaque key string. cert_out is left untouched.
    (void)cert_out;
    return true;
}

// Build the ServerCredentials for a com gRPC server. Modes (in priority):
//   1. THEIA_COM_TLS_SLOT set → ENGINE mode: key never enters com (phase 3).
//   2. THEIA_COM_TLS_CERT set → file mode: PEM cert+key from paths (phase 1).
//   3. neither               → InsecureServerCredentials (dev default).
// Client cert is OPTIONAL in both TLS modes.
//
// `who` is the caller node name, for the one-line log.
inline std::shared_ptr<grpc::ServerCredentials> make_server_creds(
        const char* who,
        const std::string& cert_param = "",
        const std::string& key_param = "",
        const std::string& ca_param = "") {
    const std::string ca_path  = tls_path_("THEIA_COM_TLS_CA",  ca_param);
    const std::string ca_cert  = read_file_(ca_path);   // "" = no CA pinning

    // ---- mode 1: engine (key never in com) --------------------------------
    //
    // The com/crypto CERTIFICATE AGREEMENT. com does not read the cert from disk
    // and does not guess: it ASKS crypto via GetCert(slot). Only crypto's
    // explicit "no cert deployed" enables insecure mode; a crypto outage or a
    // non-absence error fails CLOSED (the supervisor restarts com, which retries
    // once crypto is up — never a silent insecure port).
    if (const char* slot = std::getenv("THEIA_COM_TLS_SLOT"); slot && *slot) {
        std::string crypto_cert;
        const CryptoCert agree = crypto_get_cert(who, slot, crypto_cert);

        if (agree == CryptoCert::NO_CERT) {
            std::fprintf(stderr, "[%s] gRPC: crypto reports NO certificate for "
                         "slot '%s' (not deployed) — running INSECURE by "
                         "agreement\n", who, slot);
            return grpc::InsecureServerCredentials();
        }
        if (agree == CryptoCert::UNREACHABLE) {
            // FAIL-CLOSED: we could not get crypto's authoritative answer, so we
            // refuse to serve rather than open an insecure port. The supervisor
            // restarts com; once crypto is reachable the agreement resolves.
            std::fprintf(stderr, "[%s] gRPC: FAIL-CLOSED — crypto did not confirm "
                         "the cert for slot '%s' (unreachable / error). Refusing "
                         "to serve insecurely; aborting for supervisor restart.\n",
                         who, slot);
            std::abort();
        }

        // CERT_OK: crypto returned the cert. Bind the ENGINE private key (so the
        // key never enters com) paired with crypto's cert.
        std::string eng_key, eng_cert_unused;
        if (!engine_mode_key(who, slot, eng_key, eng_cert_unused)) {
            std::fprintf(stderr, "[%s] gRPC: FAIL-CLOSED — crypto has a cert for "
                         "slot '%s' but the theia_crypto engine failed to load; "
                         "refusing to serve. Aborting for restart.\n", who, slot);
            std::abort();
        }
        grpc::SslServerCredentialsOptions ssl_opts(
            GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
        ssl_opts.pem_root_certs = ca_cert;
        ssl_opts.pem_key_cert_pairs.push_back({eng_key, crypto_cert});
        std::fprintf(stderr, "[%s] gRPC: TLS on (slot '%s', cert from crypto, "
                     "engine key — private key never in com)\n", who, slot);
        return grpc::SslServerCredentials(ssl_opts);
    }

    // ---- mode 2: file PEM (phase 1) ---------------------------------------
    const std::string cert_path = tls_path_("THEIA_COM_TLS_CERT", cert_param);
    if (cert_path.empty()) {
        std::fprintf(stderr, "[%s] gRPC: TLS off (no THEIA_COM_TLS_SLOT / "
                     "THEIA_COM_TLS_CERT) — InsecureServerCredentials\n", who);
        return grpc::InsecureServerCredentials();
    }
    const std::string key_path = tls_path_("THEIA_COM_TLS_KEY", key_param);
    const std::string server_cert = read_file_(cert_path);
    const std::string server_key  = read_file_(key_path);
    if (server_cert.empty() || server_key.empty()) {
        std::fprintf(stderr, "[%s] gRPC: TLS requested but cert/key unreadable "
                     "(cert=%s key=%s) — falling back to INSECURE\n",
                     who, cert_path.c_str(), key_path.c_str());
        return grpc::InsecureServerCredentials();
    }

    grpc::SslServerCredentialsOptions ssl_opts(
        GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
    ssl_opts.pem_root_certs = ca_cert;
    ssl_opts.pem_key_cert_pairs.push_back({server_key, server_cert});
    std::fprintf(stderr, "[%s] gRPC: TLS on (cert=%s, file key, client-cert "
                 "optional)\n", who, cert_path.c_str());
    return grpc::SslServerCredentials(ssl_opts);
}

}  // namespace services_com
