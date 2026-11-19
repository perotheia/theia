// com_authz — per-RPC roles for com's gRPC servers (hardening F, SPEC-COM-MTLS).
//
// On a deployed rig (THEIA_COM_TLS_REQUIRED=1) every com gRPC call carries a client certificate
// verified against the fleet CA (com_tls.hpp). This file decides WHAT that caller may do:
//
//   - the caller's ROLE comes from the verified certificate: the URI SAN `theia:<role>:<id>` (preferred),
//     else the subject OU. The fleet signer (gs-api /api/pki/sign) sets both.
//   - ONE table, kRpcTable, maps every RPC full name ("/services.com.<Service>/<Method>") to the minimal
//     role it needs. Roles are ordered rig < operator < gs. An RPC that is not in the table is DENIED.
//   - debug RPCs (raw injection: StartChild, InjectSignal, CallService, SendUds) are refused for every
//     role unless the rig runs with THEIA_COM_DEBUG_RPCS=1.
//   - Provision additionally needs the caller's CN in THEIA_COM_PROVISION_CN (default "fleet-gs").
//
// ONE authorization function, authorize(), applied by ComAuthzProcessor: a grpc AuthMetadataProcessor
// that make_server_creds() attaches to the TLS credentials of all three com servers (ComGrpcProxy :7700,
// TraceForwarder :7710, LogForwarder :7711) when THEIA_COM_TLS_REQUIRED=1. grpc runs it on every call
// before the handler, with the call's :path, and turns a non-OK status into the call's status, so a
// denied call never reaches a handler. A developer run (no REQUIRED) attaches nothing: open as before.
// The table and the role semantics are documented in services/com/README.md; the test
// //services/com/test:com_roles_test keeps the table complete against the proto.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <grpcpp/security/auth_context.h>
#include <grpcpp/security/auth_metadata_processor.h>
#include <grpcpp/support/status.h>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace services_com {

// The caller's role, ordered: a higher role may do everything a lower one may.
enum class Role : int { none = 0, rig = 1, operator_ = 2, gs = 3 };

// What an RPC needs.
enum class Need : int {
    rig = 1,          // observe: any fleet certificate (another rig, an operator, the GS)
    operator_ = 2,    // control: lifecycle, trace/log config, status reads of UCM/campaign/provisioning
    gs = 3,           // fleet management: updates, campaigns, network config
    provisioner = 4,  // gs AND the CN in THEIA_COM_PROVISION_CN
    debug = 5,        // raw injection: only with THEIA_COM_DEBUG_RPCS=1, any role
};

struct RpcRule {
    const char* method;   // "/services.com.<Service>/<Method>" — grpc's :path
    Need need;
};

// THE TABLE. Every RPC of services/com/proto/supervisor_bridge.proto, nothing else (com_roles_test).
inline constexpr RpcRule kRpcTable[] = {
    // SupervisorView — the supervisor tree, lifecycle, trace/log config
    {"/services.com.SupervisorView/Subscribe",         Need::rig},
    {"/services.com.SupervisorView/StartChild",        Need::debug},       // starts an arbitrary child spec
    {"/services.com.SupervisorView/DeleteChild",       Need::operator_},
    {"/services.com.SupervisorView/RestartChild",      Need::operator_},
    {"/services.com.SupervisorView/TerminateChild",    Need::operator_},
    {"/services.com.SupervisorView/ConfigureLogLevel", Need::operator_},
    {"/services.com.SupervisorView/ConfigureTrace",    Need::operator_},
    {"/services.com.SupervisorView/GetTraceConfig",    Need::rig},
    {"/services.com.SupervisorView/GetSystemInfo",     Need::rig},
    {"/services.com.SupervisorView/GetLogLevelConfig", Need::rig},
    {"/services.com.SupervisorView/ListMachines",      Need::rig},
    {"/services.com.SupervisorView/GetTombstone",      Need::operator_},   // crash memory: not for peers
    {"/services.com.SupervisorView/InjectSignal",      Need::debug},
    {"/services.com.SupervisorView/CallService",       Need::debug},
    // TraceStream / LogStream — the egress streams (TraceForwarder :7710, LogForwarder :7711)
    {"/services.com.TraceStream/Subscribe",            Need::rig},
    {"/services.com.LogStream/Subscribe",              Need::rig},
    // PerView — persistency
    {"/services.com.PerView/ListSchemas",              Need::rig},
    {"/services.com.PerView/Snapshot",                 Need::operator_},   // writes a backup file
    {"/services.com.PerView/GetSnapshot",              Need::operator_},   // the whole config store
    // NmView — network management
    {"/services.com.NmView/GetStatus",                 Need::rig},
    {"/services.com.NmView/WifiScan",                  Need::rig},
    {"/services.com.NmView/AddWifi",                   Need::gs},
    {"/services.com.NmView/RemoveWifi",                Need::gs},
    {"/services.com.NmView/SetVpn",                    Need::gs},
    {"/services.com.NmView/SetAutoConnect",            Need::gs},
    {"/services.com.NmView/ConfirmConfig",             Need::gs},
    {"/services.com.NmView/AbortConfig",               Need::gs},
    // Provisioning — rig identity + key material
    {"/services.com.Provisioning/Provision",           Need::provisioner},
    {"/services.com.Provisioning/GetProvisionStatus",  Need::operator_},
    // DiagView — raw UDS (can write DIDs, reset ECUs)
    {"/services.com.DiagView/SendUds",                 Need::debug},
    // UcmView — software updates
    {"/services.com.UcmView/RequestUpdate",            Need::gs},
    {"/services.com.UcmView/GetProgress",              Need::operator_},
    {"/services.com.UcmView/Confirm",                  Need::gs},
    // VucmView — vehicle campaigns
    {"/services.com.VucmView/CheckForCampaign",        Need::gs},
    {"/services.com.VucmView/GetCampaignStatus",       Need::operator_},
    {"/services.com.VucmView/Decide",                  Need::gs},
};

inline const RpcRule* find_rule(const std::string& method) {
    for (const auto& r : kRpcTable)
        if (method == r.method) return &r;
    return nullptr;
}

inline const char* role_name(Role r) {
    switch (r) {
        case Role::rig: return "rig";
        case Role::operator_: return "operator";
        case Role::gs: return "gs";
        default: return "none";
    }
}
inline const char* need_name(Need n) {
    switch (n) {
        case Need::rig: return "rig";
        case Need::operator_: return "operator";
        case Need::gs: return "gs";
        case Need::provisioner: return "gs + provisioner CN";
        case Need::debug: return "debug";
    }
    return "?";
}
inline Role parse_role(const std::string& s) {
    if (s == "rig") return Role::rig;
    if (s == "operator") return Role::operator_;
    if (s == "gs") return Role::gs;
    return Role::none;
}

// The verified caller, as far as authorization cares.
struct Caller {
    Role role = Role::none;
    std::string cn;
    std::string role_source;   // "uri" | "ou" | "" — for the log / the denial message
};

// Role from a PEM certificate: the URI SAN theia:<role>:<id> first, else the subject OU.
inline Caller caller_from_pem(const std::string& pem) {
    Caller c;
    if (pem.empty()) return c;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return c;
    X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!x) return c;

    X509_NAME* subj = X509_get_subject_name(x);
    auto subject_field = [subj](int nid) -> std::string {
        std::string out;
        int idx = subj ? X509_NAME_get_index_by_NID(subj, nid, -1) : -1;
        if (idx < 0) return out;
        ASN1_STRING* d = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subj, idx));
        unsigned char* utf8 = nullptr;
        int n = d ? ASN1_STRING_to_UTF8(&utf8, d) : -1;
        if (n > 0) out.assign(reinterpret_cast<char*>(utf8), static_cast<size_t>(n));
        if (utf8) OPENSSL_free(utf8);
        return out;
    };
    c.cn = subject_field(NID_commonName);

    auto* sans = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(x, NID_subject_alt_name, nullptr, nullptr));
    if (sans) {
        for (int i = 0; i < sk_GENERAL_NAME_num(sans); ++i) {
            const GENERAL_NAME* gn = sk_GENERAL_NAME_value(sans, i);
            if (gn->type != GEN_URI) continue;
            const ASN1_IA5STRING* u = gn->d.uniformResourceIdentifier;
            std::string uri(reinterpret_cast<const char*>(ASN1_STRING_get0_data(u)),
                            static_cast<size_t>(ASN1_STRING_length(u)));
            if (uri.rfind("theia:", 0) != 0) continue;
            const size_t a = 6, b = uri.find(':', a);
            c.role = parse_role(uri.substr(a, b == std::string::npos ? std::string::npos : b - a));
            c.role_source = "uri";
            break;   // the first theia: URI decides (the signer writes exactly one)
        }
        GENERAL_NAMES_free(sans);
    }
    if (c.role_source.empty()) {
        const std::string ou = subject_field(NID_organizationalUnitName);
        if (!ou.empty()) { c.role = parse_role(ou); c.role_source = "ou"; }
    }
    X509_free(x);
    return c;
}

// The caller of a call, from grpc's auth context (the verified peer certificate).
inline Caller caller_from_auth_context(const grpc::AuthContext* ctx) {
    if (!ctx) return {};
    auto pems = ctx->FindPropertyValues("x509_pem_cert");
    if (!pems.empty()) return caller_from_pem(std::string(pems[0].data(), pems[0].size()));
    Caller c;   // no certificate: no role; keep the CN if grpc has one
    auto cns = ctx->FindPropertyValues("x509_common_name");
    if (!cns.empty()) c.cn.assign(cns[0].data(), cns[0].size());
    return c;
}

// THEIA_COM_PROVISION_CN: comma list of CNs allowed to Provision (default "fleet-gs").
inline bool provision_cn_allowed(const std::string& peer_cn) {
    const char* allow_env = std::getenv("THEIA_COM_PROVISION_CN");
    const std::string allow = (allow_env && *allow_env) ? allow_env : "fleet-gs";
    for (size_t i = 0; !peer_cn.empty() && i <= allow.size();) {
        size_t j = allow.find(',', i);
        if (j == std::string::npos) j = allow.size();
        if (allow.compare(i, j - i, peer_cn) == 0) return true;
        i = j + 1;
    }
    return false;
}

inline bool debug_rpcs_enabled() {
    const char* e = std::getenv("THEIA_COM_DEBUG_RPCS");
    return e && std::string(e) == "1";
}

// THE authorization function: may `caller` call `method` on a deployed rig?
inline grpc::Status authorize(const std::string& method, const Caller& caller, bool debug_rpcs) {
    using grpc::Status;
    using grpc::StatusCode;
    const RpcRule* rule = find_rule(method);
    if (!rule)
        return Status(StatusCode::PERMISSION_DENIED,
                      "com: " + method + " is not in com's role table (unknown RPCs are denied)");
    const std::string who = "caller '" + caller.cn + "' (role " + role_name(caller.role) +
                            (caller.role_source.empty() ? "" : " from " + caller.role_source) + ")";
    if (caller.role == Role::none)
        return Status(StatusCode::PERMISSION_DENIED,
                      "com: " + method + " denied: the client certificate carries no fleet role "
                      "(URI SAN theia:<role>:<id> or OU rig|operator|gs); " + who);
    if (rule->need == Need::debug) {
        if (debug_rpcs) return Status::OK;
        return Status(StatusCode::PERMISSION_DENIED,
                      "com: " + method + " is a debug RPC, disabled on this rig "
                      "(THEIA_COM_DEBUG_RPCS=1 enables it); " + who);
    }
    const Role need_role = rule->need == Need::provisioner ? Role::gs : static_cast<Role>(rule->need);
    if (static_cast<int>(caller.role) < static_cast<int>(need_role))
        return Status(StatusCode::PERMISSION_DENIED,
                      "com: " + method + " needs role " + role_name(need_role) + "; " + who);
    if (rule->need == Need::provisioner && !provision_cn_allowed(caller.cn))
        return Status(StatusCode::PERMISSION_DENIED,
                      "com: " + method + " needs a provisioner certificate (CN in "
                      "THEIA_COM_PROVISION_CN, default fleet-gs); " + who);
    return Status::OK;
}

// The grpc hook: runs authorize() on every call of a server whose TLS credentials carry it.
class ComAuthzProcessor final : public grpc::AuthMetadataProcessor {
public:
    explicit ComAuthzProcessor(const char* who) : who_(who ? who : "com") {}
    bool IsBlocking() const override { return false; }   // no I/O: runs inline
    grpc::Status Process(const InputMetadata& auth_metadata, grpc::AuthContext* context,
                         OutputMetadata*, OutputMetadata*) override {
        std::string method;
        auto it = auth_metadata.find(":path");
        if (it != auth_metadata.end()) method.assign(it->second.data(), it->second.size());
        if (method.empty()) {   // fail closed: no method, no decision
            std::fprintf(stderr, "[%s] authz: call without :path — denied\n", who_);
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "com: the call carries no method path: denied");
        }
        const Caller caller = caller_from_auth_context(context);
        grpc::Status st = authorize(method, caller, debug_rpcs_enabled());
        if (!st.ok()) std::fprintf(stderr, "[%s] authz: %s\n", who_, st.error_message().c_str());
        return st;
    }
private:
    const char* who_;
};

}  // namespace services_com
