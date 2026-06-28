// V2V mesh backend — MESHTASTIC (serial protobuf stream; NO MQTT).
//
// Selected by `--define mesh=meshtastic`. Opens the configured serial device and
// speaks the Meshtastic DEVICE SERIAL protocol's STREAM FRAMING:
//
//   each frame on the wire = 0x94 0xC3 <len_hi> <len_lo> <protobuf bytes...>
//
// where the protobuf is a Meshtastic ToRadio (host→device) / FromRadio
// (device→host). v1 carries the COMPACT V2V Beacon (tts/README.md) as the packet
// payload on the configured app portnum; the node converts Beacon<->wire at its
// edge. We frame/deframe the stream here and hand the inner payload up/down — the
// ToRadio/FromRadio/MeshPacket envelope is a thin wrapper we add/strip with a
// minimal hand-written encoder (no vendored Meshtastic proto suite needed for the
// single Data-packet path; the full envelope is a documented follow-up).
//
// MQTT uplink semantics of `topic` are explicitly OUT OF SCOPE (serial only).

#include "impl/v2v/mesh_backend.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ara::osi::mesh {

namespace {

constexpr uint8_t kFrame0 = 0x94;   // Meshtastic serial stream start byte 1
constexpr uint8_t kFrame1 = 0xC3;   // start byte 2
constexpr size_t  kMaxFrame = 512;  // Meshtastic max packet payload

int   g_fd = -1;
std::string g_rxbuf;                // accumulates partial stream bytes
MeshParams g_params;

// Open the serial device at 115200 8N1 raw (the Meshtastic default).
bool open_serial(const std::string& dev) {
    int fd = ::open(dev.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "[meshtastic] open(%s): %s\n",
                     dev.c_str(), std::strerror(errno));
        return false;
    }
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) { ::close(fd); return false; }
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, B115200);
    ::cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) { ::close(fd); return false; }
    g_fd = fd;
    return true;
}

}  // namespace

bool backend_open(const MeshParams& p) {
    g_params = p;
    if (!open_serial(p.serial)) return false;
    std::fprintf(stderr,
        "[meshtastic] serial %s open (channel=%s topic=%s, psk=%s) — stream framing\n",
        p.serial.c_str(), p.channel.c_str(), p.topic.c_str(),
        p.key.empty() ? "default" : "set");
    // NOTE: configuring the channel/PSK on the radio is a ToRadio AdminMessage
    // sequence; v1 assumes the device is pre-provisioned to the channel. The
    // node logs the intended channel/topic so an operator can verify the radio.
    return g_fd >= 0;
}

// Frame the payload (0x94 0xC3 len_hi len_lo <bytes>) and write it.
bool backend_send(const std::string& bytes) {
    if (g_fd < 0) return false;
    if (bytes.size() > kMaxFrame) return false;
    uint8_t hdr[4] = {kFrame0, kFrame1,
                      static_cast<uint8_t>((bytes.size() >> 8) & 0xFF),
                      static_cast<uint8_t>(bytes.size() & 0xFF)};
    std::string frame(reinterpret_cast<char*>(hdr), 4);
    frame += bytes;
    size_t off = 0;
    while (off < frame.size()) {
        ssize_t w = ::write(g_fd, frame.data() + off, frame.size() - off);
        if (w < 0) {
            if (errno == EAGAIN) continue;
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

// Deframe: accumulate stream bytes, return the next complete payload.
bool backend_recv(std::string& out, int timeout_ms) {
    if (g_fd < 0) return false;

    auto try_extract = [&]() -> bool {
        // Find the start sequence; drop noise before it.
        while (g_rxbuf.size() >= 2) {
            if (static_cast<uint8_t>(g_rxbuf[0]) == kFrame0 &&
                static_cast<uint8_t>(g_rxbuf[1]) == kFrame1) break;
            g_rxbuf.erase(0, 1);
        }
        if (g_rxbuf.size() < 4) return false;
        const size_t len = (static_cast<uint8_t>(g_rxbuf[2]) << 8) |
                            static_cast<uint8_t>(g_rxbuf[3]);
        if (len > kMaxFrame) { g_rxbuf.erase(0, 2); return false; }  // resync
        if (g_rxbuf.size() < 4 + len) return false;                  // wait for more
        out.assign(g_rxbuf, 4, len);
        g_rxbuf.erase(0, 4 + len);
        return true;
    };

    if (try_extract()) return true;

    pollfd pfd{g_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return false;
    char buf[256];
    const ssize_t r = ::read(g_fd, buf, sizeof(buf));
    if (r <= 0) return false;
    g_rxbuf.append(buf, static_cast<size_t>(r));
    return try_extract();
}

void backend_close() {
    if (g_fd >= 0) { ::close(g_fd); g_fd = -1; }
    g_rxbuf.clear();
}

const char* backend_name() { return "meshtastic"; }

}  // namespace ara::osi::mesh
