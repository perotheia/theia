// tipc_probe — a tiny non-GUI smoke for the supervisor TIPC wire.
//
// Connects via TipcClient, prints each frame's tag + length for 3 seconds,
// then exits. Used in smoke tests where a real wx window isn't available.

#include "sup_gui/tipc_client.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    std::atomic<int> frames{0};

    sup_gui::TipcClient client(0x80020001, 0,
        [&](uint16_t tag, std::string payload) {
            ++frames;
            std::fprintf(stderr,
                         "frame #%d tag=0x%04x payload_bytes=%zu\n",
                         frames.load(), tag, payload.size());
        });
    client.start();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    client.stop();
    std::fprintf(stderr, "total frames: %d  connected=%d\n",
                 frames.load(), client.is_connected() ? 1 : 0);
    return 0;
}
