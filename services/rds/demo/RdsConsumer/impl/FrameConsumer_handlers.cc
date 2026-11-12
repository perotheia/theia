// User handler bodies for FrameConsumer — the rdsdemo READER.
//
// Two readers, one per writer PROTOTYPE: `left` (from=cam_left.frames →
// /Cam_left/frames) and `right` (from=cam_right.frames → /Cam_right/frames).
// Every 20 ms each is drained (Take → validate the FrameDescriptor + pattern +
// the writer's stamp → Release). A chunk stamped by the OTHER writer counts as
// `foreign`: that would mean the two prototypes still share one stream.

#include "lib/FrameConsumer.hh"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <cstring>
#include <functional>
#include <string>

namespace ara::rdsdemo {

namespace {
constexpr int kPeriodMs = 20;

bool open_(FrameReaderStats& st, ::ara::rds::StreamReader rx, const char* writer,
           ::theia::runtime::Logger& log) {
    st.rx = std::make_unique<::ara::rds::StreamReader>(std::move(rx));
    st.expect_tag = std::hash<std::string>{}(writer);
    if (st.rx->Connect() != ::ara::rds::RdsError::OK) {
        log.error(std::string("rds: reader Connect failed for ") + writer +
                  " (is iox-roudi up?)");
        st.rx.reset();
        return false;
    }
    log.info("rds: reader for " + std::string(writer) + " on " + st.rx->spec().service +
             "/" + st.rx->spec().instance + "/" + st.rx->spec().event);
    return true;
}

void drain_(FrameReaderStats& st, const char* name, ::theia::runtime::Logger& log) {
    if (!st.rx) return;
    for (;;) {
        auto taken = st.rx->Take();
        if (!taken.ok()) break;
        ::ara::rds::Chunk c = taken.value;
        const auto* fd = reinterpret_cast<const ::ara::rds::FrameDescriptor*>(c.data);
        bool ok = fd->format == ::ara::rds::PixelFormat::RGB8 &&
                  fd->payload_size == fd->width * fd->height * 3u;
        const auto* px = reinterpret_cast<const uint8_t*>(c.data) + sizeof(*fd);
        for (uint32_t i = 0; ok && i < fd->payload_size; ++i)
            ok = px[i] == static_cast<uint8_t>((i + fd->frame_id) & 0xFF);
        if (!ok)                                  ++st.bad;
        else if (fd->timestamp_ns != st.expect_tag) ++st.foreign;
        else { ++st.received; st.last_id = fd->frame_id; }
        st.rx->Release(std::move(c));
        if (ok && st.received > 0 && st.received % 50 == 0)
            log.info(std::string("rds: [") + name + "] received " +
                     std::to_string(st.received) + " frames, last frame_id=" +
                     std::to_string(st.last_id) + ", bad=" + std::to_string(st.bad) +
                     ", foreign=" + std::to_string(st.foreign));
    }
}
}  // namespace

void FrameConsumer::init(FrameConsumerState& s) {
    bool a = open_(s.left, make_left_reader(), "cam_left", log());
    bool b = open_(s.right, make_right_reader(), "cam_right", log());
    if (a || b) ::theia::runtime::post_info(*this, "poll");
}

void FrameConsumer::handle_info(const char* info, FrameConsumerState& s) {
    if (std::strcmp(info, "poll") != 0) return;
    drain_(s.left, "left", log());
    drain_(s.right, "right", log());
    ::theia::runtime::send_after(::theia::runtime::process_timers(), kPeriodMs,
                                 *this, "poll");
}

}  // namespace ara::rdsdemo
