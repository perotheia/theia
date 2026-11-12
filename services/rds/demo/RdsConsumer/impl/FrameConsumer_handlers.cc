// User handler bodies for FrameConsumer — the rdsdemo READER.
//
// Every 20 ms: drain the stream (Take → validate the FrameDescriptor + the test
// pattern in place → Release). Logs a running count every 50 frames.
//
// Stream identity: gen-fc keys a stream's instance on the node that DECLARES it
// ("/<Prototype>/<stream>"), so the generated make_frames_reader() would listen
// on "/Frame_consumer/frames" — which no writer publishes. A reader therefore
// names the WRITER's instance explicitly, with the writer's chunk_size/history
// (the same convention the downstream packages use).

#include "lib/FrameConsumer.hh"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <cstring>
#include <string>

namespace ara::rdsdemo {

namespace {
constexpr int kPeriodMs = 20;
constexpr const char* kWriterInstance = "/Frame_producer/frames";
constexpr uint32_t kChunk = 1048576u, kHistory = 4u;   // = FrameProducer.frames
}  // namespace

void FrameConsumer::init(FrameConsumerState& s) {
    s.frames = std::make_unique<::ara::rds::StreamReader>(
        ::ara::rds::resolve_instance(kWriterInstance, kChunk, kHistory));
    if (s.frames->Connect() != ::ara::rds::RdsError::OK) {
        log().error("rds: reader Connect failed (is iox-roudi up?) — not reading");
        s.frames.reset();
        return;
    }
    log().info(std::string("rds: reader connected on ") + kWriterInstance);
    ::theia::runtime::post_info(*this, "poll");
}

void FrameConsumer::handle_info(const char* info, FrameConsumerState& s) {
    if (std::strcmp(info, "poll") != 0 || !s.frames) return;

    for (;;) {
        auto taken = s.frames->Take();
        if (!taken.ok()) break;
        ::ara::rds::Chunk c = taken.value;
        const auto* fd = reinterpret_cast<const ::ara::rds::FrameDescriptor*>(c.data);
        bool ok = fd->format == ::ara::rds::PixelFormat::RGB8 &&
                  fd->payload_size == fd->width * fd->height * 3u;
        const auto* px = reinterpret_cast<const uint8_t*>(c.data) + sizeof(*fd);
        for (uint32_t i = 0; ok && i < fd->payload_size; ++i)
            ok = px[i] == static_cast<uint8_t>((i + fd->frame_id) & 0xFF);
        if (ok) { ++s.received; s.last_id = fd->frame_id; } else { ++s.bad; }
        s.frames->Release(std::move(c));
        if (ok && s.received % 50 == 0)
            log().info("rds: received " + std::to_string(s.received) +
                       " frames, last frame_id=" + std::to_string(s.last_id) +
                       ", bad=" + std::to_string(s.bad));
    }
    ::theia::runtime::send_after(::theia::runtime::process_timers(), kPeriodMs,
                                 *this, "poll");
}

}  // namespace ara::rdsdemo
