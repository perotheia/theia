// ara::rds StreamWriter / StreamReader — the producer / consumer handles.
//
// The handles a requires_rds node holds (one per rds_stream). They own a
// transport (iceoryx by default) and expose the loan→fill→publish (writer) /
// take→read→release (reader) zero-copy cycle. No malloc/free in the runtime
// path — the chunk IS the pool memory.

#pragma once

#include "ara/rds/raw_data_stream.h"
#include "ara/rds/transport_if.h"

#include <memory>

namespace ara::rds {

// Producer. Connect once, then Loan(size) → fill the chunk → Publish.
class StreamWriter {
public:
    explicit StreamWriter(StreamSpec spec)
        : spec_(std::move(spec)), tx_(make_writer_transport()) {}

    RdsError Connect()        { return tx_->connect(spec_); }
    Result<Chunk> Loan(uint32_t size) { return tx_->loan(size); }
    RdsError Publish(Chunk&& c)        { return tx_->publish(std::move(c)); }
    void Shutdown()           { tx_->disconnect(); }

    const StreamSpec& spec() const { return spec_; }

private:
    StreamSpec spec_;
    std::unique_ptr<IWriterTransport> tx_;
};

// Consumer. Connect once, then Take() → read the chunk → Release.
class StreamReader {
public:
    explicit StreamReader(StreamSpec spec)
        : spec_(std::move(spec)), tx_(make_reader_transport()) {}

    RdsError Connect()        { return tx_->connect(spec_); }
    Result<Chunk> Take()      { return tx_->take(); }
    void Release(Chunk&& c)   { tx_->release(std::move(c)); }
    void Shutdown()           { tx_->disconnect(); }

    // Be WOKEN instead of polling: `cb` runs each time chunks arrive on this stream
    // (iceoryx's listener, one thread per process, shared by every reader), so a
    // node needs no poll timer. Keep `cb` short and non-blocking: post to the node
    // and Take() there. One post per burst (8 cameras = 8 wake-ups) with a flag:
    //     std::atomic<bool> pending{false};                       // in the node's state
    //     for (auto& rd : readers)
    //         rd->OnData([this, &pending] {
    //             if (!pending.exchange(true)) ::theia::runtime::post_info(*this, "rds"); });
    //     // handle_info("rds"): pending = false; then Take() each reader until NO_DATA
    // Several chunks may share one call, and one call may find nothing left (a drain
    // already took it). Measured (rds_latency_bench --wake 1, 8 x 1 MiB): publish ->
    // take 0.03 ms p50 (a 2 ms poll: 1.0 ms), frame start -> last take 0.38 / 0.9 ms.
    // Call after Connect(); OnData({}) detaches (Shutdown() does too).
    RdsError OnData(std::function<void()> cb) { return tx_->on_data(std::move(cb)); }

    const StreamSpec& spec() const { return spec_; }

private:
    StreamSpec spec_;
    std::unique_ptr<IReaderTransport> tx_;
};

}  // namespace ara::rds
