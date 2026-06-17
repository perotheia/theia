#include "sup_gui/grpc_client.h"

#include "supervisor_bridge.grpc.pb.h"
#include "supervisor_bridge.pb.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace sup_gui {

namespace {
constexpr uint16_t kTagEvent       = 0x0001;
constexpr uint16_t kTagHealth      = 0x0002;
constexpr uint16_t kTagSnapshot    = 0x0003;
constexpr uint16_t kTagSystemInfo  = 0x0004;   // GetSystemInfo (host + build facts)
constexpr uint16_t kTagTraceRecord = 0x0005;   // TraceStream egress (:7710)
constexpr uint16_t kTagAccel       = 0x0006;   // SHWA AccelSample (GPU / host monitor)

std::string read_file_(const char* env) {
    const char* path = std::getenv(env);
    if (!path || !*path) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "grpc_client: %s=%s unreadable — ignoring\n",
                     env, path);
        return {};
    }
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Channel credentials, TLS or insecure — matches com's opt-in TLS + rtdb's
// _make_channel. TLS is ON when THEIA_COM_TLS_CA is set (the client trusts the
// dev CA); THEIA_COM_TLS_CLIENT_CERT/_KEY add the client identity for MUTUAL
// auth (com REQUIRES + VERIFIES it when a CA is pinned). No CA → insecure (the
// local-dev default, no flag day).
std::shared_ptr<grpc::ChannelCredentials> channel_creds() {
    const std::string ca = read_file_("THEIA_COM_TLS_CA");
    if (ca.empty()) return grpc::InsecureChannelCredentials();
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs  = ca;
    opts.pem_cert_chain  = read_file_("THEIA_COM_TLS_CLIENT_CERT");  // mTLS id
    opts.pem_private_key = read_file_("THEIA_COM_TLS_CLIENT_KEY");
    return grpc::SslCredentials(opts);
}
}  // namespace

GrpcClient::GrpcClient(std::string machine_name,
                        std::string host_port,
                        FrameCallback on_frame)
    : machine_name_(std::move(machine_name)),
      host_port_(std::move(host_port)),
      callback_(std::move(on_frame)) {}

GrpcClient::~GrpcClient() { stop(); }

void GrpcClient::start() {
    if (running_.exchange(true)) return;
    thread_       = std::thread([this] { run(); });
    trace_thread_ = std::thread([this] { run_trace(); });
}

void GrpcClient::stop() {
    if (!running_.exchange(false)) return;
    // CANCEL any in-flight stream Read() — resetting the channel alone does
    // NOT (the reader keeps its own ref). Without this, the trace thread hangs
    // in reader->Read() until the next record, which never arrives when no
    // trace is enabled → Quit hangs on the join below.
    {
        std::lock_guard<std::mutex> lk(ctx_mu_);
        if (sub_ctx_)
            static_cast<grpc::ClientContext*>(sub_ctx_)->TryCancel();
        if (trace_ctx_)
            static_cast<grpc::ClientContext*>(trace_ctx_)->TryCancel();
    }
    channel_.reset();
    trace_channel_.reset();
    if (thread_.joinable())       thread_.join();
    if (trace_thread_.joinable()) trace_thread_.join();
}

// SupervisorView is host:7700; TraceStream (com's TraceForwarder) is host:7710.
// Swap the port; honor an explicit override.
std::string GrpcClient::trace_endpoint() const {
    if (const char* e = std::getenv("THEIA_COM_TRACE_LISTEN")) return e;
    const auto colon = host_port_.rfind(':');
    const std::string host = (colon == std::string::npos)
                                 ? host_port_ : host_port_.substr(0, colon);
    return host + ":7710";
}

// TraceStream egress: subscribe to com's TraceForwarder and post each
// TraceRecord to the panel as tag 0x0005. The records are the live message
// traces (node→node casts/calls) that `tdb logcat` / `rtdb logcat` show — a
// SEPARATE stream + port from SupervisorView, so its own thread + channel.
void GrpcClient::run_trace() {
    const std::string endpoint = trace_endpoint();
    while (running_.load()) {
        trace_channel_ = grpc::CreateChannel(
            endpoint, channel_creds());
        auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(
            trace_channel_);
        auto stub = ::services::com::TraceStream::NewStub(ci);
        ::services::com::TraceSubscribeRequest req;   // kind=0, all nodes
        grpc::ClientContext ctx;
        { std::lock_guard<std::mutex> lk(ctx_mu_); trace_ctx_ = &ctx; }
        auto reader = stub->Subscribe(&ctx, req);

        ::services::com::TraceRecord rec;
        while (running_.load() && reader->Read(&rec)) {
            if (callback_)
                callback_(machine_name_, kTagTraceRecord,
                          rec.SerializeAsString());
        }
        reader->Finish();
        { std::lock_guard<std::mutex> lk(ctx_mu_); trace_ctx_ = nullptr; }
        if (running_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int GrpcClient::configure_trace(const std::string& target_node,
                                const std::string& msg_type,
                                bool enabled, uint32_t kind) {
    // Use a fresh short-lived channel to keep the streaming thread's
    // channel untouched. The trace CONTROL path (enable/disable a node's
    // tracer) is ConfigureTrace on SupervisorView (:7700) — the SAME RPC
    // rtdb / tdb drive. (TraceStream is the EGRESS stream on :7710, a
    // separate service; control stays on SupervisorView.)
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);

    ::services::com::TraceConfigRequest req;
    req.set_target_node(target_node);
    req.set_msg_type(msg_type);
    req.set_enabled(enabled);
    req.set_kind(kind);   // TraceKind (0 = all kinds) — the supervisor keys the
                          // per-node trace filter on kind (a bitmask), #403.

    ::system_supervisor::ControlReply rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(3));
    auto status = stub->ConfigureTrace(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr,
            "grpc_client[%s]: configure_trace RPC failed: %s\n",
            machine_name_.c_str(), status.error_message().c_str());
        return -1;
    }
    return static_cast<int>(rep.status());
}

// ---- Persistency proxy (PerView on the SAME :7700 SupervisorView endpoint) --

std::vector<GrpcClient::SchemaRow>
GrpcClient::list_schemas(const std::string& config_type, bool* ok) {
    std::vector<SchemaRow> out;
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::PerView::NewStub(ci);
    ::services::com::ListSchemasCall req;
    req.set_config_type(config_type);
    ::services::com::PerSchemaList rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    auto status = stub->ListSchemas(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: ListSchemas failed: %s\n",
                     machine_name_.c_str(), status.error_message().c_str());
        if (ok) *ok = false;
        return out;
    }
    for (const auto& s : rep.schemas())
        out.push_back({s.config_type(), s.digest()});
    if (ok) *ok = true;
    return out;
}

int GrpcClient::snapshot(const std::string& label, std::string* msg) {
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::PerView::NewStub(ci);
    ::services::com::SnapshotCall req;
    req.set_label(label);
    ::services::com::PerReply rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto status = stub->Snapshot(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: Snapshot failed: %s\n",
                     machine_name_.c_str(), status.error_message().c_str());
        if (msg) *msg = status.error_message();
        return -1;
    }
    if (msg) *msg = rep.message();
    return static_cast<int>(rep.status());
}

std::vector<GrpcClient::StoreRow>
GrpcClient::get_store_snapshot(const std::string& config_type, bool* ok) {
    std::vector<StoreRow> out;
    auto chan = grpc::CreateChannel(host_port_, channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::PerView::NewStub(ci);
    ::services::com::GetSnapshotCall req;
    req.set_config_type(config_type);
    ::services::com::PerStoreSnapshot rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto status = stub->GetSnapshot(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: GetSnapshot failed: %s\n",
                     machine_name_.c_str(), status.error_message().c_str());
        if (ok) *ok = false;
        return out;
    }
    for (const auto& r : rep.rows())
        out.push_back({r.config_type(), r.digest(), r.config()});
    if (ok) *ok = true;
    return out;
}

// ---- crash forensics: GetTombstone (Applications panel right-click) ------
// GetTombstone on SupervisorView (:7700). com forwards to the supervisor's
// TIPC op and returns the (capped) tombstone bytes. Synchronous unary, fresh
// channel. A valid found=false reply is NOT an error (sets *ok=true).
GrpcClient::TombstoneResult
GrpcClient::get_tombstone(const std::string& child_name, bool* ok) {
    TombstoneResult out;
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);
    ::services::com::GetTombstoneCall req;
    req.set_child_name(child_name);
    ::services::com::GetTombstoneReply rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto status = stub->GetTombstone(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: GetTombstone failed: %s\n",
                     machine_name_.c_str(), status.error_message().c_str());
        if (ok) *ok = false;
        return out;
    }
    out.found       = rep.found();
    out.truncated   = rep.truncated();
    out.total_bytes = rep.total_bytes();
    out.path        = rep.path();
    out.content     = rep.content();
    if (ok) *ok = true;
    return out;
}

// ---- live log-level (Applications panel right-click → Log level) --------
// ConfigureLogLevel on SupervisorView (:7700). The supervisor stores the
// override AND pushes it live to the node (same RPC `rtdb loglevel <n> <lvl>`
// drives). Synchronous unary, fresh channel.
int GrpcClient::configure_log_level(const std::string& target_node,
                                    const std::string& level) {
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);
    ::services::com::LogLevelCall req;
    req.set_target_node(target_node);
    req.set_level(level);
    ::system_supervisor::ControlReply rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    auto status = stub->ConfigureLogLevel(&ctx, req, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: ConfigureLogLevel failed: %s\n",
                     machine_name_.c_str(), status.error_message().c_str());
        return -1;
    }
    return static_cast<int>(rep.status());
}

// Read ONE node's current effective log level (the Applications-panel Log-level
// submenu checkmark). GetLogLevelConfig returns every reporting node's status;
// we find the row for `node` and map its level ordinal to a name. "" when the
// node isn't found (not reporting yet) or the RPC fails.
std::string GrpcClient::get_log_level(const std::string& node) {
    static const char* kLvl[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);
    ::services::com::GetLogLevelConfigCall req;
    ::system_supervisor::LogLevelConfigList rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    if (!stub->GetLogLevelConfig(&ctx, req, &rep).ok()) return {};
    for (const auto& s : rep.configs()) {
        if (s.target_node() == node) {
            int lv = static_cast<int>(s.level());
            return (lv >= 0 && lv < 5) ? kLvl[lv] : "";
        }
    }
    return {};
}

// ---- child lifecycle (Processes panel right-click) ----------------------
// Kill = RestartChild (no_restart=false): kill + supervisor restarts it.
// Remove = TerminateChild (no_restart=true): stop-and-hold, no policy restart.
// Both are ChildSelector → ControlReply on SupervisorView. Synchronous unary,
// fresh channel (don't touch the stream threads).
int GrpcClient::restart_child(const std::string& name, std::string* msg) {
    return child_op(name, /*no_restart=*/false, "RestartChild", msg);
}
int GrpcClient::terminate_child(const std::string& name, std::string* msg) {
    return child_op(name, /*no_restart=*/true, "TerminateChild", msg);
}
int GrpcClient::child_op(const std::string& name, bool no_restart,
                         const char* which, std::string* msg) {
    auto chan = grpc::CreateChannel(host_port_,
                                    channel_creds());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);
    ::system_supervisor::ChildSelector sel;
    sel.set_name(name);
    sel.set_no_restart(no_restart);
    ::system_supervisor::ControlReply rep;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    grpc::Status status = no_restart ? stub->TerminateChild(&ctx, sel, &rep)
                                     : stub->RestartChild(&ctx, sel, &rep);
    if (!status.ok()) {
        std::fprintf(stderr, "grpc_client[%s]: %s(%s) failed: %s\n",
                     machine_name_.c_str(), which, name.c_str(),
                     status.error_message().c_str());
        if (msg) *msg = status.error_message();
        return -1;
    }
    if (msg) *msg = rep.message();
    return static_cast<int>(rep.status());
}

void GrpcClient::run() {
    while (running_.load()) {
        channel_ = grpc::CreateChannel(host_port_,
                                        channel_creds());
        // NewStub wants shared_ptr<ChannelInterface>. Channel inherits
        // from it but the implicit conversion isn't picked up in
        // grpc 1.30 — go through static_pointer_cast.
        auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(channel_);
        auto stub = ::services::com::SupervisorView::NewStub(ci);
        ::services::com::SubscribeRequest req;
        grpc::ClientContext ctx;
        { std::lock_guard<std::mutex> lk(ctx_mu_); sub_ctx_ = &ctx; }
        auto reader = stub->Subscribe(&ctx, req);

        connected_.store(true);
        std::fprintf(stderr, "grpc_client[%s]: subscribed to %s\n",
                     machine_name_.c_str(), host_port_.c_str());

        // One-shot host + build facts on connect (the `tdb info` surface). It's
        // per-boot-static (hostname/kernel/ram fixed; sha/build_ts/started per
        // supervisor process), so polling it once per (re)connect is enough.
        // GetSystemInfo is a unary RPC on the SAME SupervisorView stub.
        {
            ::services::com::GetSystemInfoCall si_req;
            ::system_supervisor::SystemInfo si;
            grpc::ClientContext si_ctx;
            si_ctx.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::seconds(3));
            if (stub->GetSystemInfo(&si_ctx, si_req, &si).ok() && callback_) {
                callback_(machine_name_, kTagSystemInfo, si.SerializeAsString());
            }
        }

        ::services::com::SupervisorObservation obs;
        while (running_.load() && reader->Read(&obs)) {
            uint16_t tag = 0;
            std::string payload;
            switch (obs.kind_case()) {
                case ::services::com::SupervisorObservation::kEvent:
                    tag = kTagEvent;
                    payload = obs.event().SerializeAsString();
                    break;
                case ::services::com::SupervisorObservation::kHealth:
                    tag = kTagHealth;
                    payload = obs.health().SerializeAsString();
                    break;
                case ::services::com::SupervisorObservation::kSnapshot:
                    tag = kTagSnapshot;
                    payload = obs.snapshot().SerializeAsString();
                    break;
                case ::services::com::SupervisorObservation::kAccel:
                    tag = kTagAccel;
                    payload = obs.accel().SerializeAsString();
                    break;
                default:
                    continue;
            }
            if (callback_) callback_(machine_name_, tag, std::move(payload));
        }
        reader->Finish();
        { std::lock_guard<std::mutex> lk(ctx_mu_); sub_ctx_ = nullptr; }
        connected_.store(false);
        std::fprintf(stderr, "grpc_client[%s]: stream ended\n",
                     machine_name_.c_str());
        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

}  // namespace sup_gui
