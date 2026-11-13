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

// The iceoryx C binding (1.0.1) has no user-payload-size getter; chunk_size.cc
// asks the C++ ChunkHeader (its header cannot share a TU with the C binding).
namespace ara::rds::detail { uint32_t chunk_payload_size(const void* user_payload); }

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

namespace ara::rds {

// ---- the latency trace (opt-in: THEIA_RDS_TRACE=<dir>) ------------------------
// One CSV per process, <dir>/rds-trace-<pid>.csv, one line per chunk:
//   ev,stream,frame_id,t0_ns,t1_ns
//   pub   t0 = Loan() returned, t1 = Publish() called       (writer)
//   take  t0 = Take() returned, t1 = Release() called       (reader)
// frame_id = the chunk's FrameDescriptor.frame_id (the chunk is one), else 0.
// CLOCK_REALTIME ns, the clock of spr's timing CSV, so the two join on frame_id.
// Off (unset), it costs one branch per chunk.
namespace {
uint64_t real_ns() {
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
struct Trace {
    FILE* f = nullptr;
    std::mutex mu;
    Trace() {
        const char* d = std::getenv("THEIA_RDS_TRACE");
        if (!d || !*d) return;
        char path[512];
        std::snprintf(path, sizeof(path), "%s/rds-trace-%d.csv", d, int(getpid()));
        f = std::fopen(path, "w");
        if (f) std::fprintf(f, "ev,stream,frame_id,t0_ns,t1_ns\n");
    }
    void put(const char* ev, const std::string& stream, const void* data, uint32_t size,
             uint64_t t0, uint64_t t1) {
        const uint64_t id = (data && size >= sizeof(FrameDescriptor))
                                ? static_cast<const FrameDescriptor*>(data)->frame_id : 0;
        std::lock_guard<std::mutex> l(mu);
        std::fprintf(f, "%s,%s,%llu,%llu,%llu\n", ev, stream.c_str(), (unsigned long long)id,
                     (unsigned long long)t0, (unsigned long long)t1);
        std::fflush(f);
    }
};
Trace& trace() { static Trace t; return t; }
}  // namespace

void Runtime::Init(const std::string& runtime_name) {
    // RouDi is host-wide and refuses a second runtime of one name (it ABORTS the
    // newcomer: IPC_INTERFACE__APP_WITH_SAME_NAME_STILL_RUNNING). An isolated
    // stack (`theia start --instance N`, N != 0: THEIA_PG_SUPERVISOR_INSTANCE=N,
    // the pg rule) qualifies its names, "<name>@<N>", so two stacks may run the
    // same composition. The default stack and a rig keep the bare name.
    const std::string n = stack_qualified(runtime_name);
    // iceoryx runtime names are capped (iox::RUNTIME_MAX_APP_NAME_LENGTH, 100):
    // truncate defensively.
    char name[100];
    std::snprintf(name, sizeof(name), "%s", n.c_str());
    iox_runtime_init(name);
}

namespace {

class IceoryxWriter final : public IWriterTransport {
public:
    RdsError connect(const StreamSpec& s) override {
        iox_pub_options_t opts;
        iox_pub_options_init(&opts);
        opts.historyCapacity = s.history;
        const std::string service = stack_qualified(s.service);   // "<Service>@N" on stack N != 0
        pub_ = iox_pub_init(&storage_, service.c_str(), s.instance.c_str(),
                            s.event.c_str(), &opts);
        name_ = "/" + s.service + "/" + s.instance;
        return pub_ ? RdsError::OK : RdsError::TRANSPORT_ERROR;
    }
    Result<Chunk> loan(uint32_t size) override {
        if (!pub_) return Result<Chunk>::bad(RdsError::NOT_CONNECTED);
        void* payload = nullptr;
        if (iox_pub_loan_chunk(pub_, &payload, size) != AllocationResult_SUCCESS)
            return Result<Chunk>::bad(RdsError::LOAN_FAILED);
        Chunk c; c.data = payload; c.size = size; c.handle = payload;
        if (trace().f) { loaned_ns_ = real_ns(); }
        return Result<Chunk>::good(c);
    }
    RdsError publish(Chunk&& c) override {
        if (!pub_) return RdsError::NOT_CONNECTED;
        if (!c.valid()) return RdsError::BAD_STREAM;
        if (trace().f) trace().put("pub", name_, c.data, c.size, loaned_ns_, real_ns());
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
    std::string name_;            // the logical stream, for the trace
    uint64_t loaned_ns_ = 0;      // the trace: the last Loan (one writer = one thread)
};

class IceoryxReader final : public IReaderTransport {
public:
    RdsError connect(const StreamSpec& s) override {
        iox_sub_options_t opts;
        iox_sub_options_init(&opts);
        opts.historyRequest = s.history;
        opts.queueCapacity = s.history ? s.history : 4;
        const std::string service = stack_qualified(s.service);   // "<Service>@N" on stack N != 0
        sub_ = iox_sub_init(&storage_, service.c_str(), s.instance.c_str(),
                            s.event.c_str(), &opts);
        name_ = "/" + s.service + "/" + s.instance;
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
        // The loaned size, as the writer asked for it (a FrameDescriptor reader
        // may still read its own payload_size; a generic reader needs this).
        c.size = ara::rds::detail::chunk_payload_size(payload);
        if (trace().f) c.taken_ns = real_ns();
        return Result<Chunk>::good(c);
    }
    void release(Chunk&& c) override {
        if (trace().f && c.handle) trace().put("take", name_, c.handle, c.size, c.taken_ns, real_ns());
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
    std::string name_;            // the logical stream, for the trace
};

}  // namespace

std::unique_ptr<IWriterTransport> make_writer_transport() {
    return std::make_unique<IceoryxWriter>();
}
std::unique_ptr<IReaderTransport> make_reader_transport() {
    return std::make_unique<IceoryxReader>();
}

}  // namespace ara::rds
