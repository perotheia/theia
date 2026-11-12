// User handler bodies for FrameProducer — the rdsdemo WRITER.
//
// Every 100 ms: Loan a chunk from the RouDi pool, write a FrameDescriptor + a
// small RGB8 test pattern into it in place, Publish. Zero copy, no malloc in
// the data path. The consumer (RdsConsumer/FrameConsumer) Takes the same chunk.
//
// Stream identity: make_frames_writer() (a member of the node) publishes on
// "/<Prototype>/frames", keyed on this node's RUNTIME prototype name. The
// RdsProducer composition prototypes this node twice (cam_left, cam_right), so
// the process publishes TWO streams: /Cam_left/frames and /Cam_right/frames.
// Each frame's timestamp_ns carries a hash of the prototype name so the
// consumer can prove which writer a chunk came from.

#include "lib/FrameProducer.hh"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <cstring>
#include <functional>
#include <string>

namespace ara::rdsdemo {

namespace {
constexpr int      kPeriodMs = 100;
constexpr uint32_t kW = 64, kH = 48;          // RGB8 test frame
constexpr uint32_t kPayload = kW * kH * 3;
}  // namespace

void FrameProducer::init(FrameProducerState& s) {
    s.frames = std::make_unique<::ara::rds::StreamWriter>(make_frames_writer());
    s.source_tag = std::hash<std::string>{}(node_name());
    const auto rc = s.frames->Connect();
    if (rc != ::ara::rds::RdsError::OK) {
        log().error("rds: writer Connect failed (is iox-roudi up?) — not publishing");
        s.frames.reset();
        return;
    }
    log().info("rds: writer connected on " + s.frames->spec().service + "/" +
               s.frames->spec().instance + "/" + s.frames->spec().event);
    ::theia::runtime::post_info(*this, "tick");
}

void FrameProducer::handle_info(const char* info, FrameProducerState& s) {
    if (std::strcmp(info, "tick") != 0 || !s.frames) return;

    auto loaned = s.frames->Loan(sizeof(::ara::rds::FrameDescriptor) + kPayload);
    if (!loaned.ok()) {
        ++s.loan_fail;
    } else {
        ::ara::rds::Chunk c = loaned.value;
        auto* fd = reinterpret_cast<::ara::rds::FrameDescriptor*>(c.data);
        fd->frame_id     = s.next_frame_id;
        fd->width        = kW;
        fd->height       = kH;
        fd->format       = ::ara::rds::PixelFormat::RGB8;
        fd->timestamp_ns = s.source_tag;   // which writer (prototype) this is
        fd->payload_size = kPayload;
        auto* px = reinterpret_cast<uint8_t*>(c.data) + sizeof(*fd);
        for (uint32_t i = 0; i < kPayload; ++i)
            px[i] = static_cast<uint8_t>((i + s.next_frame_id) & 0xFF);
        if (s.frames->Publish(std::move(c)) == ::ara::rds::RdsError::OK) {
            ++s.published;
            ++s.next_frame_id;
        }
    }
    if (s.next_frame_id % 50 == 0 && s.published > 0)
        log().info("rds: [" + node_name() + "] published " + std::to_string(s.published) +
                   " frames (loan failures " + std::to_string(s.loan_fail) + ")");
    ::theia::runtime::send_after(::theia::runtime::process_timers(), kPeriodMs,
                                 *this, "tick");
}

}  // namespace ara::rdsdemo
