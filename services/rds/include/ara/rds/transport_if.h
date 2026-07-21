// ara::rds::ITransport — the transport abstraction that hides iceoryx.
//
// StreamWriter/StreamReader talk ONLY to this interface; the concrete transport
// (IceoryxTransport today; TcpTransport / UdpTransport later) is chosen by the
// deployment without changing application code. The producer side loans + writes
// shared memory (Loan/Publish), the consumer side reads it (Take/Release) —
// zero-copy for the shared-memory transport.

#pragma once

#include "ara/rds/raw_data_stream.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ara::rds {

// A resolved stream endpoint — the deployment triple (iceoryx service/instance/
// event) + the pool sizing, produced by the deployment resolver from the
// manifest topic name. Applications never construct this; gen-fc/the resolver
// does, from an InstanceSpecifier like "/Perception/CameraFront".
struct StreamSpec {
    std::string service;     // iceoryx service   (e.g. "Perception")
    std::string instance;    // iceoryx instance  (e.g. "CameraFront")
    std::string event;       // iceoryx event     (e.g. "frame")
    uint32_t    max_chunk = 0;   // max user-payload bytes (the pool chunk size)
    uint32_t    history = 0;     // history depth (chunks retained for late joiners)
};

// The producer half of a transport. Loan a chunk, fill it, Publish it.
class IWriterTransport {
public:
    virtual ~IWriterTransport() = default;
    virtual RdsError connect(const StreamSpec& s) = 0;
    virtual Result<Chunk> loan(uint32_t size) = 0;
    virtual RdsError publish(Chunk&& c) = 0;
    virtual void disconnect() = 0;
};

// The consumer half. Take a chunk (a pointer into the pool), read it, Release it.
class IReaderTransport {
public:
    virtual ~IReaderTransport() = default;
    virtual RdsError connect(const StreamSpec& s) = 0;
    virtual Result<Chunk> take() = 0;            // NO_DATA if nothing queued
    virtual void release(Chunk&& c) = 0;
    virtual void disconnect() = 0;
};

// Factory — the deployment picks the transport. v1: iceoryx only.
std::unique_ptr<IWriterTransport> make_writer_transport();
std::unique_ptr<IReaderTransport> make_reader_transport();

// Resolve an InstanceSpecifier ("/Perception/CameraFront") → a StreamSpec
// (iceoryx triple + pool sizing). chunk_size/history come from the manifest
// `rds_stream` declaration (gen-fc passes them); 0 = defaults.
StreamSpec resolve_instance(const std::string& instance_specifier,
                            uint32_t max_chunk = 0, uint32_t history = 0);

}  // namespace ara::rds
