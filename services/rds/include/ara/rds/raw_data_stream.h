// ara::rds — Raw Data Stream: zero-copy bulk transport for the data plane.
//
// The COMPLEMENT to Theia's TIPC control plane: TIPC carries small messages
// (casts/calls/the firehose, "frame N ready") and copies; RDS carries BULK
// payloads (an 8 MB camera frame) through iceoryx SHARED MEMORY with ZERO copy —
// the producer loans a chunk, fills it, publishes; every consumer takes a
// pointer to the SAME memory. RouDi (iox-roudi, a supervised native daemon)
// owns the pools + does discovery.
//
// This is the app-facing API (close to AUTOSAR ara::rds, plus zero-copy loan
// semantics). A node opts in via the .art `requires_rds` flag + `rds_stream`
// declarations; gen-fc links this lib, calls Runtime::Init, and emits a typed
// StreamWriter/StreamReader per stream. The transport is hidden behind
// ITransport (iceoryx today; Tcp/Udp later) so applications never see iceoryx.
//
// See docs/autosar/services/rds.md + docs/tasks/DONE/Raw-Data-Stream.md.

#pragma once

#include <cstdint>
#include <string>

namespace ara::rds {

// Result — a minimal AUTOSAR-style outcome (no exceptions in the data path).
enum class RdsError {
    OK = 0,
    NOT_CONNECTED,
    LOAN_FAILED,      // pool exhausted / chunk too big
    NO_DATA,          // Take with nothing available
    BAD_STREAM,       // unknown topic / not initialized
    TRANSPORT_ERROR,
};

template <typename T>
struct Result {
    RdsError error = RdsError::OK;
    T        value{};
    bool     ok() const { return error == RdsError::OK; }
    static Result good(T v) { return {RdsError::OK, std::move(v)}; }
    static Result bad(RdsError e) { return {e, {}}; }
};

// A loaned shared-memory chunk — `data` points INTO the iceoryx pool (zero copy).
// The producer fills it before Publish; the consumer reads it before Release.
// Never owns the memory: the pool does. `size` is the user-payload size.
struct Chunk {
    void*    data = nullptr;
    uint32_t size = 0;
    void*    handle = nullptr;   // opaque (the iceoryx chunk header) — don't touch
    bool     valid() const { return data != nullptr; }
};

// One process registers ONCE with RouDi under a unique runtime name (the node
// name). gen-fc calls this from main() for a requires_rds node.
struct Runtime {
    static void Init(const std::string& runtime_name);
};

// ─── Frame descriptors (the ticket's camera model) ──────────────────────────

enum class PixelFormat : uint32_t { UNKNOWN = 0, RGB8, BGR8, YUV420, NV12, GRAY8 };

// CPU frame: descriptor + payload in ONE chunk (descriptor, then image bytes).
struct FrameDescriptor {
    uint64_t    frame_id = 0;
    uint32_t    width = 0;
    uint32_t    height = 0;
    PixelFormat format = PixelFormat::UNKNOWN;
    uint64_t    timestamp_ns = 0;
    uint32_t    payload_size = 0;   // bytes of image data following this header
    // [ image payload ] immediately follows in the same chunk.
};

// GPU frame (Orin): iceoryx transports ONLY this descriptor; the image stays in
// NvMedia/CUDA/DMA-BUF memory referenced by dma_buf_fd. No image copy. The fd is
// passed out-of-band (SCM_RIGHTS) on a capable host — declared here; the real
// DMA-BUF zero-copy is the Orin follow-up.
struct GpuFrameDescriptor {
    uint64_t    frame_id = 0;
    int         dma_buf_fd = -1;
    uint32_t    width = 0;
    uint32_t    height = 0;
    PixelFormat format = PixelFormat::UNKNOWN;
    uint64_t    timestamp_ns = 0;
};

}  // namespace ara::rds
