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
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include <openssl/engine.h>   // engine-mode TLS key (phase 3)

namespace services_com {

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
    // The cert (public) comes from the slot dir.
    const char* dir = std::getenv("THEIA_CRYPTO_SLOT_DIR");
    cert_out = read_file_((std::string(dir ? dir : "/etc/theia/crypto") + "/" +
                           slot + ".crt"));
    return !cert_out.empty();
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
    if (const char* slot = std::getenv("THEIA_COM_TLS_SLOT"); slot && *slot) {
        std::string eng_key, eng_cert;
        if (engine_mode_key(who, slot, eng_key, eng_cert)) {
            grpc::SslServerCredentialsOptions ssl_opts(
                GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
            ssl_opts.pem_root_certs = ca_cert;
            ssl_opts.pem_key_cert_pairs.push_back({eng_key, eng_cert});
            std::fprintf(stderr, "[%s] gRPC: TLS on (slot '%s' via crypto FC "
                         "engine — private key never in com)\n", who, slot);
            return grpc::SslServerCredentials(ssl_opts);
        }
        std::fprintf(stderr, "[%s] gRPC: TLS slot '%s' engine setup failed — "
                     "falling back to INSECURE\n", who, slot);
        return grpc::InsecureServerCredentials();
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
