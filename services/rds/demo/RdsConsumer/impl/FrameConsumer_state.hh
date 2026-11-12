// State struct for FrameConsumer — APP-OWNED, WRITE-ONCE.

#pragma once

#include "ara/rds/stream.h"

#include <cstdint>
#include <memory>

namespace ara::rdsdemo {

// One reader per writer prototype (the .art declares `left` from=cam_left.frames
// and `right` from=cam_right.frames).
struct FrameReaderStats {
    std::unique_ptr<::ara::rds::StreamReader> rx;
    uint64_t expect_tag = 0;   // hash of the writer prototype this stream must carry
    uint64_t received   = 0;   // chunks Taken + validated
    uint64_t bad        = 0;   // descriptor/payload failed to validate
    uint64_t foreign    = 0;   // chunk stamped by the OTHER writer (streams mixed)
    uint64_t last_id    = 0;
};

struct FrameConsumerState {
    FrameReaderStats left, right;
};

}  // namespace ara::rdsdemo
