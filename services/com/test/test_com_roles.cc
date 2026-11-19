// com_roles_test — per-RPC roles of com's gRPC servers (hardening F, impl/com_authz.hpp).
//
// 1. TABLE: every RPC of services/com/proto/supervisor_bridge.proto is in kRpcTable, and the table names
//    no RPC the proto does not have (a new RPC without a role fails here, not in the field).
// 2. ROLES, end to end: a throwaway fleet CA + certificates for rig, operator and gs (and a few odd ones)
//    are generated here with OpenSSL; an in-process server is built with com's REAL credentials
//    (services_com::make_server_creds, THEIA_COM_TLS_REQUIRED=1, file mode) and every service of the
//    proto registered UNIMPLEMENTED. A call the role check lets through therefore ends UNIMPLEMENTED;
//    a refused one ends PERMISSION_DENIED before any handler.
// 3. DEVELOPER RUN: without THEIA_COM_TLS_REQUIRED the same server stays open (no role check).
//
// No stack, loopback only. Exit 0 = pass.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/byte_buffer.h>

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "impl/com_authz.hpp"
#include "impl/com_tls.hpp"
#include "supervisor_bridge.grpc.pb.h"

namespace sc = services_com;
using grpc::StatusCode;

static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); ++failures; } } while (0)

// ── certificates ──────────────────────────────────────────────────────────

static EVP_PKEY* make_key() {
    EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* k = nullptr;
    EVP_PKEY_keygen_init(kc);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kc, NID_X9_62_prime256v1);
    EVP_PKEY_keygen(kc, &k);
    EVP_PKEY_CTX_free(kc);
    return k;
}

static void add_ext(X509* x, X509* issuer, int nid, const char* value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, x, nullptr, nullptr, 0);
    X509_EXTENSION* e = X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char*>(value));
    if (!e) { std::fprintf(stderr, "bad extension %d=%s\n", nid, value); std::abort(); }
    X509_add_ext(x, e, -1);
    X509_EXTENSION_free(e);
}

struct Cert {
    EVP_PKEY* key = nullptr;
    X509* x = nullptr;
    std::string key_pem, cert_pem;
};

static std::string to_pem(X509* x) {
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(b, x);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string s(d, static_cast<size_t>(n));
    BIO_free(b);
    return s;
}
static std::string to_pem(EVP_PKEY* k) {
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
    char* d = nullptr;
    long n = BIO_get_mem_data(b, &d);
    std::string s(d, static_cast<size_t>(n));
    BIO_free(b);
    return s;
}

// cn / ou ("" = none) / san ("" = none, else an OpenSSL SAN list); issuer null = self-signed CA.
static Cert make_cert(const std::string& cn, const std::string& ou, const std::string& san,
                      const Cert* issuer) {
    static long serial = 1;
    Cert c;
    c.key = make_key();
    c.x = X509_new();
    X509_set_version(c.x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(c.x), serial++);
    X509_gmtime_adj(X509_getm_notBefore(c.x), -300);
    X509_gmtime_adj(X509_getm_notAfter(c.x), 3600);
    X509_set_pubkey(c.x, c.key);
    X509_NAME* n = X509_get_subject_name(c.x);
    X509_NAME_add_entry_by_txt(n, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("perotheia fleet"), -1, -1, 0);
    if (!ou.empty())
        X509_NAME_add_entry_by_txt(n, "OU", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(ou.c_str()), -1, -1, 0);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    X509* iss = issuer ? issuer->x : c.x;
    X509_set_issuer_name(c.x, X509_get_subject_name(iss));
    if (!issuer) {
        add_ext(c.x, c.x, NID_basic_constraints, "critical,CA:TRUE");
        add_ext(c.x, c.x, NID_key_usage, "critical,keyCertSign,cRLSign");
        add_ext(c.x, c.x, NID_subject_key_identifier, "hash");
    } else {
        add_ext(c.x, iss, NID_basic_constraints, "critical,CA:FALSE");
        add_ext(c.x, iss, NID_key_usage, "critical,digitalSignature,keyAgreement");
        add_ext(c.x, iss, NID_ext_key_usage, "clientAuth,serverAuth");
        add_ext(c.x, iss, NID_authority_key_identifier, "keyid:always");
        if (!san.empty()) add_ext(c.x, iss, NID_subject_alt_name, san.c_str());
    }
    X509_sign(c.x, issuer ? issuer->key : c.key, EVP_sha256());
    c.key_pem = to_pem(c.key);
    c.cert_pem = to_pem(c.x);
    return c;
}

static std::string write_tmp(const std::string& name, const std::string& body) {
    const char* d = std::getenv("TEST_TMPDIR");
    std::string p = std::string(d && *d ? d : "/tmp") + "/com_roles_" + name;
    std::ofstream(p, std::ios::binary) << body;
    return p;
}

// ── server + calls ────────────────────────────────────────────────────────

// Every service of the bridge proto, all methods UNIMPLEMENTED (the base classes).
struct Services {
    services::com::SupervisorView::Service sup;
    services::com::TraceStream::Service trace;
    services::com::LogStream::Service log;
    services::com::PerView::Service per;
    services::com::NmView::Service nm;
    services::com::Provisioning::Service prov;
    services::com::DiagView::Service diag;
    services::com::UcmView::Service ucm;
    services::com::VucmView::Service vucm;
};

static std::unique_ptr<grpc::Server> start_server(Services& s, int* port) {
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", sc::make_server_creds("com_roles_test"), port);
    b.RegisterService(&s.sup);  b.RegisterService(&s.trace); b.RegisterService(&s.log);
    b.RegisterService(&s.per);  b.RegisterService(&s.nm);    b.RegisterService(&s.prov);
    b.RegisterService(&s.diag); b.RegisterService(&s.ucm);   b.RegisterService(&s.vucm);
    return b.BuildAndStart();
}

static std::shared_ptr<grpc::Channel> channel_as(int port, const std::string& ca, const Cert& who) {
    grpc::SslCredentialsOptions o;
    o.pem_root_certs = ca;
    o.pem_private_key = who.key_pem;
    o.pem_cert_chain = who.cert_pem;
    grpc::ChannelArguments args;
    args.SetSslTargetNameOverride("rig.fleet.theia");   // the rig verified by its fleet name
    return grpc::CreateCustomChannel("127.0.0.1:" + std::to_string(port),
                                     grpc::SslCredentials(o), args);
}

struct Result { StatusCode code; std::string msg; };

// One call of `method` with an empty request (every request message of the proto is valid empty).
static Result call(const std::shared_ptr<grpc::Channel>& ch, const std::string& method) {
    grpc::GenericStub stub(ch);
    grpc::CompletionQueue cq;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    grpc::ByteBuffer req, resp;
    {
        grpc::Slice empty("", 0);
        req = grpc::ByteBuffer(&empty, 1);
    }
    grpc::Status st;
    auto rpc = stub.PrepareUnaryCall(&ctx, method, req, &cq);
    rpc->StartCall();
    rpc->Finish(&resp, &st, reinterpret_cast<void*>(1));
    void* tag = nullptr;
    bool ok = false;
    cq.Next(&tag, &ok);
    cq.Shutdown();
    while (cq.Next(&tag, &ok)) {}
    return {st.error_code(), st.error_message()};
}

static bool allowed(const Result& r) { return r.code == StatusCode::UNIMPLEMENTED; }
static bool denied(const Result& r) { return r.code == StatusCode::PERMISSION_DENIED; }

static std::vector<std::string> proto_methods() {
    std::vector<std::string> out;
    const auto* file = services::com::ListMachinesCall::descriptor()->file();
    for (int i = 0; i < file->service_count(); ++i) {
        const auto* svc = file->service(i);
        for (int j = 0; j < svc->method_count(); ++j)
            out.push_back("/" + svc->full_name() + "/" + svc->method(j)->name());
    }
    return out;
}

int main() {
    // ── 1. table ↔ proto ──────────────────────────────────────────────────
    const auto methods = proto_methods();
    CHECK(methods.size() >= 30, "proto has only %zu RPCs?", methods.size());
    for (const auto& m : methods)
        CHECK(sc::find_rule(m) != nullptr, "RPC %s is not in com's role table (kRpcTable)", m.c_str());
    for (const auto& r : sc::kRpcTable) {
        bool in_proto = false;
        for (const auto& m : methods) in_proto = in_proto || m == r.method;
        CHECK(in_proto, "kRpcTable names %s, which the proto does not have", r.method);
        int dup = 0;
        for (const auto& r2 : sc::kRpcTable) dup += std::string(r.method) == r2.method;
        CHECK(dup == 1, "kRpcTable lists %s %d times", r.method, dup);
    }

    // ── 2. certificates ───────────────────────────────────────────────────
    const Cert ca = make_cert("perotheia fleet CA", "", "", nullptr);
    const Cert server = make_cert("rig-a", "rig", "URI:theia:rig:rig-a,DNS:rig-a,DNS:rig.fleet.theia", &ca);
    const Cert rig = make_cert("rig-b", "rig", "URI:theia:rig:rig-b,DNS:rig-b,DNS:rig.fleet.theia", &ca);
    const Cert op = make_cert("alice", "operator", "URI:theia:operator:alice,DNS:alice,DNS:operator.fleet.theia", &ca);
    const Cert gs = make_cert("fleet-gs", "gs", "URI:theia:gs:fleet-gs,DNS:fleet-gs,DNS:gs.fleet.theia", &ca);
    // odd ones
    const Cert gs_other = make_cert("gs-2", "gs", "URI:theia:gs:gs-2,DNS:gs-2", &ca);              // gs, not a provisioner CN
    const Cert op_as_gs = make_cert("fleet-gs", "operator", "URI:theia:operator:fleet-gs", &ca);   // provisioner CN, operator role
    const Cert ou_only = make_cert("bob", "operator", "DNS:bob", &ca);                              // role from OU
    const Cert uri_wins = make_cert("mallory", "gs", "URI:theia:rig:mallory", &ca);                 // URI says rig, OU says gs
    const Cert no_role = make_cert("nobody", "", "DNS:nobody", &ca);                                 // fleet-signed, no role

    // caller_from_pem
    CHECK(sc::caller_from_pem(rig.cert_pem).role == sc::Role::rig, "rig cert -> rig");
    CHECK(sc::caller_from_pem(op.cert_pem).role == sc::Role::operator_, "operator cert -> operator");
    CHECK(sc::caller_from_pem(gs.cert_pem).role == sc::Role::gs, "gs cert -> gs");
    CHECK(sc::caller_from_pem(gs.cert_pem).cn == "fleet-gs", "gs cert CN");
    CHECK(sc::caller_from_pem(ou_only.cert_pem).role == sc::Role::operator_ &&
          sc::caller_from_pem(ou_only.cert_pem).role_source == "ou", "OU fallback");
    CHECK(sc::caller_from_pem(uri_wins.cert_pem).role == sc::Role::rig &&
          sc::caller_from_pem(uri_wins.cert_pem).role_source == "uri", "URI SAN preferred over OU");
    CHECK(sc::caller_from_pem(no_role.cert_pem).role == sc::Role::none, "no role");

    const std::string ca_path = write_tmp("ca.crt", ca.cert_pem);
    const std::string crt_path = write_tmp("server.crt", server.cert_pem);
    const std::string key_path = write_tmp("server.key", server.key_pem);
    ::unsetenv("THEIA_COM_TLS_SLOT");
    ::unsetenv("THEIA_COM_INSECURE");
    ::unsetenv("THEIA_COM_DEBUG_RPCS");
    ::unsetenv("THEIA_COM_PROVISION_CN");
    ::setenv("THEIA_COM_TLS_CA", ca_path.c_str(), 1);
    ::setenv("THEIA_COM_TLS_CERT", crt_path.c_str(), 1);
    ::setenv("THEIA_COM_TLS_KEY", key_path.c_str(), 1);

    // ── 3. deployed rig: THEIA_COM_TLS_REQUIRED=1 ─────────────────────────
    ::setenv("THEIA_COM_TLS_REQUIRED", "1", 1);
    {
        Services svcs;
        int port = 0;
        auto srv = start_server(svcs, &port);
        CHECK(srv && port > 0, "server did not start");
        if (!srv) return 1;

        auto ch_rig = channel_as(port, ca.cert_pem, rig);
        auto ch_op = channel_as(port, ca.cert_pem, op);
        auto ch_gs = channel_as(port, ca.cert_pem, gs);

        const char* observe[] = {
            "/services.com.SupervisorView/Subscribe", "/services.com.SupervisorView/ListMachines",
            "/services.com.SupervisorView/GetSystemInfo", "/services.com.SupervisorView/GetTraceConfig",
            "/services.com.SupervisorView/GetLogLevelConfig", "/services.com.TraceStream/Subscribe",
            "/services.com.LogStream/Subscribe", "/services.com.PerView/ListSchemas",
            "/services.com.NmView/GetStatus", "/services.com.NmView/WifiScan"};
        const char* control[] = {
            "/services.com.SupervisorView/RestartChild", "/services.com.SupervisorView/TerminateChild",
            "/services.com.SupervisorView/DeleteChild", "/services.com.SupervisorView/ConfigureLogLevel",
            "/services.com.SupervisorView/ConfigureTrace", "/services.com.SupervisorView/GetTombstone",
            "/services.com.UcmView/GetProgress", "/services.com.VucmView/GetCampaignStatus",
            "/services.com.PerView/GetSnapshot", "/services.com.Provisioning/GetProvisionStatus"};
        const char* fleet[] = {
            "/services.com.UcmView/RequestUpdate", "/services.com.UcmView/Confirm",
            "/services.com.VucmView/CheckForCampaign", "/services.com.VucmView/Decide",
            "/services.com.NmView/SetVpn", "/services.com.NmView/AddWifi"};
        const char* debug[] = {
            "/services.com.SupervisorView/StartChild", "/services.com.SupervisorView/InjectSignal",
            "/services.com.SupervisorView/CallService", "/services.com.DiagView/SendUds"};
        const char* kProvision = "/services.com.Provisioning/Provision";

        for (const char* m : observe) {
            CHECK(allowed(call(ch_rig, m)), "rig: %s must be allowed", m);
            CHECK(allowed(call(ch_op, m)), "operator: %s must be allowed", m);
            CHECK(allowed(call(ch_gs, m)), "gs: %s must be allowed", m);
        }
        for (const char* m : control) {
            const Result r = call(ch_rig, m);
            CHECK(denied(r), "rig: %s must be PERMISSION_DENIED (got %d %s)", m, (int)r.code, r.msg.c_str());
            CHECK(r.msg.find("needs role operator") != std::string::npos, "rig denial message: %s", r.msg.c_str());
            CHECK(allowed(call(ch_op, m)), "operator: %s must be allowed", m);
            CHECK(allowed(call(ch_gs, m)), "gs: %s must be allowed", m);
        }
        for (const char* m : fleet) {
            CHECK(denied(call(ch_rig, m)), "rig: %s must be denied", m);
            CHECK(denied(call(ch_op, m)), "operator: %s must be denied", m);
            CHECK(allowed(call(ch_gs, m)), "gs: %s must be allowed", m);
        }
        // Provision: gs AND a provisioner CN
        CHECK(denied(call(ch_rig, kProvision)), "rig: Provision denied");
        CHECK(denied(call(ch_op, kProvision)), "operator: Provision denied");
        CHECK(allowed(call(ch_gs, kProvision)), "gs fleet-gs: Provision allowed");
        {
            const Result r = call(channel_as(port, ca.cert_pem, gs_other), kProvision);
            CHECK(denied(r) && r.msg.find("provisioner") != std::string::npos,
                  "gs with CN gs-2: Provision denied by the CN gate (%s)", r.msg.c_str());
            CHECK(allowed(call(channel_as(port, ca.cert_pem, gs_other), "/services.com.UcmView/RequestUpdate")),
                  "gs with CN gs-2: RequestUpdate allowed");
        }
        CHECK(denied(call(channel_as(port, ca.cert_pem, op_as_gs), kProvision)),
              "operator role with CN fleet-gs: Provision denied (role, not CN alone)");
        ::setenv("THEIA_COM_PROVISION_CN", "gs-2,fleet-gs", 1);
        CHECK(allowed(call(channel_as(port, ca.cert_pem, gs_other), kProvision)),
              "THEIA_COM_PROVISION_CN=gs-2,fleet-gs: gs-2 may Provision");
        ::unsetenv("THEIA_COM_PROVISION_CN");

        // debug RPCs: off for every role, on for every role with THEIA_COM_DEBUG_RPCS=1
        for (const char* m : debug) {
            for (auto* ch : {&ch_rig, &ch_op, &ch_gs}) {
                const Result r = call(*ch, m);
                CHECK(denied(r) && r.msg.find("THEIA_COM_DEBUG_RPCS") != std::string::npos,
                      "%s must be denied without THEIA_COM_DEBUG_RPCS (got %d %s)", m, (int)r.code, r.msg.c_str());
            }
        }
        ::setenv("THEIA_COM_DEBUG_RPCS", "1", 1);
        for (const char* m : debug)
            for (auto* ch : {&ch_rig, &ch_op, &ch_gs})
                CHECK(allowed(call(*ch, m)), "%s must be allowed with THEIA_COM_DEBUG_RPCS=1", m);
        ::unsetenv("THEIA_COM_DEBUG_RPCS");

        // unknown RPCs: denied, even for gs
        CHECK(denied(call(ch_gs, "/services.com.SupervisorView/NoSuchRpc")), "unknown method denied");
        CHECK(denied(call(ch_gs, "/grpc.health.v1.Health/Check")), "unknown service denied");

        // role sources
        CHECK(allowed(call(channel_as(port, ca.cert_pem, ou_only), "/services.com.SupervisorView/RestartChild")),
              "OU-only operator cert may RestartChild");
        CHECK(denied(call(channel_as(port, ca.cert_pem, uri_wins), "/services.com.SupervisorView/RestartChild")),
              "URI SAN rig beats OU gs");
        {
            const Result r = call(channel_as(port, ca.cert_pem, no_role), "/services.com.SupervisorView/ListMachines");
            CHECK(denied(r) && r.msg.find("no fleet role") != std::string::npos,
                  "a fleet cert without a role is denied everything (%s)", r.msg.c_str());
        }

        // every RPC of the proto, for every role: the wire answer is authorize()'s answer
        for (const auto& m : methods) {
            for (const Cert* c : {&rig, &op, &gs}) {
                const bool want = sc::authorize(m, sc::caller_from_pem(c->cert_pem), false).ok();
                const Result r = call(channel_as(port, ca.cert_pem, *c), m);
                CHECK(want ? allowed(r) : denied(r), "%s as %s: wire %d vs authorize %d",
                      m.c_str(), c == &rig ? "rig" : c == &op ? "operator" : "gs", (int)r.code, want);
            }
        }
        srv->Shutdown();
    }

    // ── 4. developer run: no REQUIRED → open (mutual TLS still, no role check) ──
    ::unsetenv("THEIA_COM_TLS_REQUIRED");
    {
        Services svcs;
        int port = 0;
        auto srv = start_server(svcs, &port);
        CHECK(srv && port > 0, "dev server did not start");
        if (srv) {
            auto ch_rig = channel_as(port, ca.cert_pem, rig);
            CHECK(allowed(call(ch_rig, "/services.com.SupervisorView/StartChild")), "dev run: StartChild open");
            CHECK(allowed(call(ch_rig, "/services.com.Provisioning/Provision")), "dev run: Provision open");
            CHECK(allowed(call(ch_rig, "/services.com.UcmView/RequestUpdate")), "dev run: RequestUpdate open");
            srv->Shutdown();
        }
    }

    std::fprintf(stderr, "com_roles_test: %d checks, %d failures (%zu RPCs in the proto)\n",
                 checks, failures, methods.size());
    return failures == 0 ? 0 : 1;
}
