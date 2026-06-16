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

    const StreamSpec& spec() const { return spec_; }

private:
    StreamSpec spec_;
    std::unique_ptr<IReaderTransport> tx_;
};

}  // namespace ara::rds
