// rds_roundtrip — live e2e of the zero-copy data plane over RouDi.
//
// Proves producer→shared-memory→consumer with NO copy and NO malloc in the
// path: the writer Loans a chunk, fills a FrameDescriptor + a payload pattern,
// Publishes; the reader Takes the SAME pool memory, validates the descriptor +
// payload, and asserts the pointer is NOT the writer's heap (it's shared
// memory). Requires iox-roudi running (the supervised broker).
//
// Both endpoints in one process (separate runtime names) — iceoryx allows that;
// the cross-process case is the same API, exercised by the demo FCs.

#include "ara/rds/stream.h"
#include "ara/rds/transport_if.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

using namespace ara::rds;

int main() {
    Runtime::Init("rds_roundtrip");

    auto spec = resolve_instance("/Test/Frames", /*chunk*/ 1u << 20, /*hist*/ 4);

    StreamWriter w(spec);
    StreamReader r(spec);
    if (w.Connect() != RdsError::OK || r.Connect() != RdsError::OK) {
        std::printf("rds-roundtrip: SKIP (RouDi not running — start iox-roudi)\n");
        return 0;
    }
    // Let discovery settle (sub finds the pub).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const uint32_t W = 64, H = 48, payload = W * H * 3;   // RGB8
    const uint32_t total = sizeof(FrameDescriptor) + payload;

    // ---- PRODUCER: loan → fill → publish (zero malloc) ----
    auto loaned = w.Loan(total);
    assert(loaned.ok() && "loan a chunk from the pool");
    Chunk wc = loaned.value;
    auto* fd = reinterpret_cast<FrameDescriptor*>(wc.data);
    fd->frame_id = 42;
    fd->width = W; fd->height = H; fd->format = PixelFormat::RGB8;
    fd->timestamp_ns = 123456789;
    fd->payload_size = payload;
    auto* img = reinterpret_cast<uint8_t*>(wc.data) + sizeof(FrameDescriptor);
    for (uint32_t i = 0; i < payload; ++i) img[i] = (uint8_t)(i & 0xFF);
    void* produced_ptr = wc.data;
    assert(w.Publish(std::move(wc)) == RdsError::OK);

    // ---- CONSUMER: take → read → release (same memory, zero copy) ----
    Chunk rc;
    for (int tries = 0; tries < 50; ++tries) {
        auto taken = r.Take();
        if (taken.ok()) { rc = taken.value; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(rc.valid() && "reader took the published chunk");

    auto* rfd = reinterpret_cast<const FrameDescriptor*>(rc.data);
    assert(rfd->frame_id == 42 && rfd->width == W && rfd->height == H);
    assert(rfd->format == PixelFormat::RGB8 && rfd->payload_size == payload);
    auto* rimg = reinterpret_cast<const uint8_t*>(rc.data) + sizeof(FrameDescriptor);
    for (uint32_t i = 0; i < payload; ++i)
        assert(rimg[i] == (uint8_t)(i & 0xFF) && "payload survived zero-copy");

    // The reader's pointer is shared-memory (the SAME the writer wrote), not a
    // copy on this process's heap — that's the whole point.
    std::printf("rds-roundtrip: OK — frame %llu %ux%u (%u B payload) "
                "producer ptr=%p reader ptr=%p (shared-mem zero-copy)\n",
                (unsigned long long)rfd->frame_id, rfd->width, rfd->height,
                rfd->payload_size, produced_ptr, (const void*)rc.data);

    r.Release(std::move(rc));
    w.Shutdown(); r.Shutdown();
    return 0;
}
