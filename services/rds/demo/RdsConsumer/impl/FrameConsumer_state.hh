// State struct for FrameConsumer — APP-OWNED, WRITE-ONCE.

#pragma once

#include "ara/rds/stream.h"

#include <cstdint>
#include <memory>

namespace ara::rdsdemo {

struct FrameConsumerState {
    std::unique_ptr<::ara::rds::StreamReader> frames;  // reads the producer's stream
    uint64_t received = 0;   // chunks Taken + validated
    uint64_t bad      = 0;   // chunks whose descriptor/payload did not validate
    uint64_t last_id  = 0;   // frame_id of the last chunk Taken
};

}  // namespace ara::rdsdemo
