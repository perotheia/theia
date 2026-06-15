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
#include <sstream>
#include <string>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

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

// Build the ServerCredentials for a com gRPC server. `cert_param`/`key_param`/
// `ca_param` are the com-param values (may be empty); env vars override them.
// Returns InsecureServerCredentials when no cert is configured (dev default),
// else SslServerCredentials with OPTIONAL client-cert verification.
//
// `who` is the caller node name, for the one-line log.
inline std::shared_ptr<grpc::ServerCredentials> make_server_creds(
        const char* who,
        const std::string& cert_param = "",
        const std::string& key_param = "",
        const std::string& ca_param = "") {
    const std::string cert_path = tls_path_("THEIA_COM_TLS_CERT", cert_param);
    if (cert_path.empty()) {
        std::fprintf(stderr, "[%s] gRPC: TLS off (no THEIA_COM_TLS_CERT / "
                     "tls_cert param) — InsecureServerCredentials\n", who);
        return grpc::InsecureServerCredentials();
    }
    const std::string key_path = tls_path_("THEIA_COM_TLS_KEY", key_param);
    const std::string ca_path  = tls_path_("THEIA_COM_TLS_CA",  ca_param);

    const std::string server_cert = read_file_(cert_path);
    const std::string server_key  = read_file_(key_path);
    const std::string ca_cert     = read_file_(ca_path);   // "" = no CA pinning
    if (server_cert.empty() || server_key.empty()) {
        std::fprintf(stderr, "[%s] gRPC: TLS requested but cert/key unreadable "
                     "(cert=%s key=%s) — falling back to INSECURE\n",
                     who, cert_path.c_str(), key_path.c_str());
        return grpc::InsecureServerCredentials();
    }

    grpc::SslServerCredentialsOptions ssl_opts(
        // Optional client cert (v1 policy): always encrypt; identify a client
        // that presents a cert, but let cert-less clients connect. Strict mTLS
        // would be GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY.
        GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY);
    ssl_opts.pem_root_certs = ca_cert;
    ssl_opts.pem_key_cert_pairs.push_back({server_key, server_cert});

    std::fprintf(stderr, "[%s] gRPC: TLS on (cert=%s, client-cert optional)\n",
                 who, cert_path.c_str());
    return grpc::SslServerCredentials(ssl_opts);
}

}  // namespace services_com
