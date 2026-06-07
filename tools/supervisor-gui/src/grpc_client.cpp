#include "sup_gui/grpc_client.h"

#include "supervisor_bridge.grpc.pb.h"
#include "supervisor_bridge.pb.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace sup_gui {

namespace {
constexpr uint16_t kTagEvent       = 0x0001;
constexpr uint16_t kTagHealth      = 0x0002;
constexpr uint16_t kTagSnapshot    = 0x0003;
constexpr uint16_t kTagSystemInfo  = 0x0004;   // GetSystemInfo (host + build facts)
constexpr uint16_t kTagTraceRecord = 0x0005;   // TraceStream egress (:7710)
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
    // Channel shutdown wakes any blocked Read() on either stream.
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
            endpoint, grpc::InsecureChannelCredentials());
        auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(
            trace_channel_);
        auto stub = ::services::com::TraceStream::NewStub(ci);
        ::services::com::TraceSubscribeRequest req;   // kind=0, all nodes
        grpc::ClientContext ctx;
        auto reader = stub->Subscribe(&ctx, req);

        ::services::com::TraceRecord rec;
        while (running_.load() && reader->Read(&rec)) {
            if (callback_)
                callback_(machine_name_, kTagTraceRecord,
                          rec.SerializeAsString());
        }
        reader->Finish();
        if (running_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int GrpcClient::configure_trace(const std::string& target_node,
                                const std::string& msg_type,
                                bool enabled) {
    // Use a fresh short-lived channel to keep the streaming thread's
    // channel untouched. The trace CONTROL path (enable/disable a node's
    // tracer) is ConfigureTrace on SupervisorView (:7700) — the SAME RPC
    // rtdb / tdb drive. (TraceStream is the EGRESS stream on :7710, a
    // separate service; control stays on SupervisorView.)
    auto chan = grpc::CreateChannel(host_port_,
                                    grpc::InsecureChannelCredentials());
    auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(chan);
    auto stub = ::services::com::SupervisorView::NewStub(ci);

    ::services::com::TraceConfigRequest req;
    req.set_target_node(target_node);
    req.set_msg_type(msg_type);
    req.set_enabled(enabled);

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
                                    grpc::InsecureChannelCredentials());
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
                                    grpc::InsecureChannelCredentials());
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

void GrpcClient::run() {
    while (running_.load()) {
        channel_ = grpc::CreateChannel(host_port_,
                                        grpc::InsecureChannelCredentials());
        // NewStub wants shared_ptr<ChannelInterface>. Channel inherits
        // from it but the implicit conversion isn't picked up in
        // grpc 1.30 — go through static_pointer_cast.
        auto ci = std::static_pointer_cast< ::grpc::ChannelInterface>(channel_);
        auto stub = ::services::com::SupervisorView::NewStub(ci);
        ::services::com::SubscribeRequest req;
        grpc::ClientContext ctx;
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
                default:
                    continue;
            }
            if (callback_) callback_(machine_name_, tag, std::move(payload));
        }
        reader->Finish();
        connected_.store(false);
        std::fprintf(stderr, "grpc_client[%s]: stream ended\n",
                     machine_name_.c_str());
        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

}  // namespace sup_gui
