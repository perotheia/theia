#include "sup_gui/grpc_client.h"

#include "supervisor_bridge.grpc.pb.h"
#include "supervisor_bridge.pb.h"

#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace sup_gui {

namespace {
constexpr uint16_t kTagEvent    = 0x0001;
constexpr uint16_t kTagHealth   = 0x0002;
constexpr uint16_t kTagSnapshot = 0x0003;
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
    thread_ = std::thread([this] { run(); });
}

void GrpcClient::stop() {
    if (!running_.exchange(false)) return;
    // Channel shutdown wakes any blocked Read().
    channel_.reset();
    if (thread_.joinable()) thread_.join();
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
