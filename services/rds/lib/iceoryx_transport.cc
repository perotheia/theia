// IceoryxTransport — the ITransport implementation over the iceoryx C binding.
//
// Hides iceoryx from applications: Loan/Publish → iox_pub_loan_chunk /
// iox_pub_publish_chunk; Take/Release → iox_sub_take_chunk /
// iox_sub_release_chunk. Zero-copy — the loaned/taken pointer IS pool memory.
// Runtime::Init → iox_runtime_init (register the process with RouDi).
//
// The iceoryx service triple comes from the resolved StreamSpec (the deployment
// resolver mapped the manifest topic to it). iceoryx caps service/instance/event
// at 100 chars + the pool chunk size at init.

#include "ara/rds/transport_if.h"
#include "ara/rds/raw_data_stream.h"

extern "C" {
#include "iceoryx_binding_c/runtime.h"
#include "iceoryx_binding_c/publisher.h"
#include "iceoryx_binding_c/subscriber.h"
#include "iceoryx_binding_c/enums.h"
#include "iceoryx_binding_c/types.h"
}

#include <cstring>

namespace ara::rds {

void Runtime::Init(const std::string& runtime_name) {
    // iceoryx runtime names are capped (iox::RUNTIME_MAX_APP_NAME_LENGTH). Trust
    // the node name to fit; truncate defensively.
    char name[100];
    std::snprintf(name, sizeof(name), "%s", runtime_name.c_str());
    iox_runtime_init(name);
}

namespace {

class IceoryxWriter final : public IWriterTransport {
public:
    RdsError connect(const StreamSpec& s) override {
        iox_pub_options_t opts;
        iox_pub_options_init(&opts);
        opts.historyCapacity = s.history;
        pub_ = iox_pub_init(&storage_, s.service.c_str(), s.instance.c_str(),
                            s.event.c_str(), &opts);
        return pub_ ? RdsError::OK : RdsError::TRANSPORT_ERROR;
    }
    Result<Chunk> loan(uint32_t size) override {
        if (!pub_) return Result<Chunk>::bad(RdsError::NOT_CONNECTED);
        void* payload = nullptr;
        if (iox_pub_loan_chunk(pub_, &payload, size) != AllocationResult_SUCCESS)
            return Result<Chunk>::bad(RdsError::LOAN_FAILED);
        Chunk c; c.data = payload; c.size = size; c.handle = payload;
        return Result<Chunk>::good(c);
    }
    RdsError publish(Chunk&& c) override {
        if (!pub_) return RdsError::NOT_CONNECTED;
        if (!c.valid()) return RdsError::BAD_STREAM;
        iox_pub_publish_chunk(pub_, c.data);
        c.data = nullptr;   // ownership handed to the pool
        return RdsError::OK;
    }
    void disconnect() override {
        if (pub_) { iox_pub_deinit(pub_); pub_ = nullptr; }
    }
    ~IceoryxWriter() override { disconnect(); }

private:
    iox_pub_storage_t storage_{};
    iox_pub_t pub_ = nullptr;
};

class IceoryxReader final : public IReaderTransport {
public:
    RdsError connect(const StreamSpec& s) override {
        iox_sub_options_t opts;
        iox_sub_options_init(&opts);
        opts.historyRequest = s.history;
        opts.queueCapacity = s.history ? s.history : 4;
        sub_ = iox_sub_init(&storage_, s.service.c_str(), s.instance.c_str(),
                            s.event.c_str(), &opts);
        if (!sub_) return RdsError::TRANSPORT_ERROR;
        iox_sub_subscribe(sub_);
        return RdsError::OK;
    }
    Result<Chunk> take() override {
        if (!sub_) return Result<Chunk>::bad(RdsError::NOT_CONNECTED);
        const void* payload = nullptr;
        auto rc = iox_sub_take_chunk(sub_, &payload);
        if (rc != ChunkReceiveResult_SUCCESS)
            return Result<Chunk>::bad(RdsError::NO_DATA);
        Chunk c;
        c.data = const_cast<void*>(payload);
        c.handle = const_cast<void*>(payload);
        c.size = 0;   // size is carried in the app's FrameDescriptor header
        return Result<Chunk>::good(c);
    }
    void release(Chunk&& c) override {
        if (sub_ && c.handle) iox_sub_release_chunk(sub_, c.handle);
        c.data = nullptr; c.handle = nullptr;
    }
    void disconnect() override {
        if (sub_) { iox_sub_unsubscribe(sub_); iox_sub_deinit(sub_); sub_ = nullptr; }
    }
    ~IceoryxReader() override { disconnect(); }

private:
    iox_sub_storage_t storage_{};
    iox_sub_t sub_ = nullptr;
};

}  // namespace

std::unique_ptr<IWriterTransport> make_writer_transport() {
    return std::make_unique<IceoryxWriter>();
}
std::unique_ptr<IReaderTransport> make_reader_transport() {
    return std::make_unique<IceoryxReader>();
}

}  // namespace ara::rds
