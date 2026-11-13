// The user-payload size of a received iceoryx chunk (HAND-OWNED).
//
// Separate TU on purpose: iceoryx_posh's C++ headers and the C binding's headers
// do not compile together, and the 1.0.1 C binding exposes no size getter.
#include "iceoryx_posh/mepoo/chunk_header.hpp"

#include <cstdint>

namespace ara::rds::detail {

uint32_t chunk_payload_size(const void* user_payload) {
    if (!user_payload) return 0;
    return iox::mepoo::ChunkHeader::fromUserPayload(user_payload)->userPayloadSize();
}

}  // namespace ara::rds::detail
