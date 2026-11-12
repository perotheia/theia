// State struct for FrameProducer — APP-OWNED, WRITE-ONCE.

#pragma once

#include "ara/rds/stream.h"

#include <cstdint>
#include <memory>

namespace ara::rdsdemo {

struct FrameProducerState {
    // The writer handle for the `frames` stream (rds_streams::make_frames_writer).
    std::unique_ptr<::ara::rds::StreamWriter> frames;
    uint64_t next_frame_id = 0;   // frame_id of the next published frame
    uint64_t published     = 0;   // frames successfully published
    uint64_t loan_fail     = 0;   // Loan() failures (pool exhausted / no RouDi)
    uint64_t source_tag    = 0;   // hash of the prototype name, stamped in each frame
};

}  // namespace ara::rdsdemo
