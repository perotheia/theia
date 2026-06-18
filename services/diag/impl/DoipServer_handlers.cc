// DoipServer — the DoIP (ISO 13400) TCP server. HAND-OWNED.
//
// A `runnable` (owns its thread + the TCP/13400 socket, no watchdog — like com's
// ComGrpcProxy). do_loop accepts a tester connection, frames DoIP, decodes the
// routing-activation handshake + each diagnostic message, hands the raw UDS to
// UdsRouter over TIPC (RemoteRef call UdsIf.Handle), and re-frames the reply.
// The UDS semantics (session/security/DID/DTC) live in UdsRouter; this node is
// purely the transport edge — exactly the com gRPC↔TIPC bridge shape.

#include "lib/DoipServer.hh"
#include "lib/diag_codecs.hh"      // RemoteCodec<UdsRequest/UdsReply>
#include "impl/doip.hpp"          // the DoIP wire model

#include "TipcMux.hh"             // reply pump for the RemoteRef
#include "NodeRef.hh"            // RemoteRef + theia::runtime::call

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ara::diag {

namespace {

// UdsRouter's identity from the DoIP server's side (RemoteRef needs a NodeType
// only for the trace tag + destination). Address from the .art (0x80010018).
struct UdsRouterTag { static constexpr const char* kNodeName = "uds_router"; };
using UdsRef = theia::runtime::RemoteRef<UdsRouterTag, 0x80010018u, 0u>;

constexpr uint16_t kDoipPort   = 13400;   // ISO 13400 default
constexpr uint16_t kEcuAddress = 0x0001;  // our DoIP logical address

// Server runtime state (the runnable owns one instance via the static below).
struct ServerState {
    int                     listen_fd = -1;
    std::atomic<bool>       up{false};
    theia::runtime::TipcMux mux;     // reply pump for the UdsRouter RemoteRef
    UdsRef                  ref;     // the call handle into UdsRouter
    bool                    ref_up = false;
};
ServerState& S() { static ServerState s; return s; }

// Call UdsRouter.Handle(UdsRequest) over TIPC, return the UDS response bytes.
// Best-effort: on a transport failure return false (the caller frames a UDS
// general-reject NRC).
bool call_uds_router(const doip::Bytes& uds, uint16_t source, uint16_t target,
                     doip::Bytes& out) {
    system_services_diag_UdsRequest req = system_services_diag_UdsRequest_init_zero;
    req.source_addr = source;
    req.target_addr = target;
    req.uds.size = static_cast<pb_size_t>(
        uds.size() > sizeof(req.uds.bytes) ? sizeof(req.uds.bytes) : uds.size());
    std::memcpy(req.uds.bytes, uds.data(), req.uds.size);

    auto result = theia::runtime::call<system_services_diag_UdsReply>(
        S().ref, req, /*act=*/0, /*timeout_ms=*/3000);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    const auto& rep = result.reply;
    out.assign(rep.uds.bytes, rep.uds.bytes + rep.uds.size);
    return true;
}

// Read one DoIP message off `fd` into `buf` (accumulating partial TCP reads).
// Returns true with a complete `msg`, false on EOF/error/malformed.
bool recv_doip(int fd, std::vector<uint8_t>& buf, doip::Message& msg) {
    for (;;) {
        size_t consumed = 0;
        bool need_more = false;
        if (!buf.empty() &&
            doip::parse(buf.data(), buf.size(), msg, consumed, need_more)) {
            buf.erase(buf.begin(), buf.begin() + consumed);
            return true;
        }
        if (!need_more && !buf.empty()) {   // malformed → drop the connection
            std::fprintf(stderr, "[doip_server] malformed DoIP frame — closing\n");
            return false;
        }
        uint8_t tmp[2048];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;           // EOF / error
        buf.insert(buf.end(), tmp, tmp + n);
    }
}

inline bool send_all(int fd, const doip::Bytes& b) {
    size_t off = 0;
    while (off < b.size()) {
        ssize_t n = ::send(fd, b.data() + off, b.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Serve ONE accepted tester connection: routing activation, then a loop of
// diagnostic messages (each → UdsRouter → UDS response). Closes on EOF/error.
void serve_connection(int cfd) {
    std::vector<uint8_t> buf;
    uint16_t tester_addr = 0;
    bool activated = false;

    for (;;) {
        doip::Message m;
        if (!recv_doip(cfd, buf, m)) break;

        switch (m.type) {
        case doip::PT_RoutingActivReq: {
            doip::RoutingActivReq r;
            uint8_t code = doip::RA_UnknownSource;
            if (doip::parse_routing_activ(m.payload, r)) {
                tester_addr = r.source_addr;
                activated = true;
                code = doip::RA_Success;
            }
            send_all(cfd, doip::make_routing_activ_resp(tester_addr, kEcuAddress, code));
            std::fprintf(stderr, "[doip_server] routing activation tester=0x%04X → %s\n",
                         tester_addr, code == doip::RA_Success ? "OK" : "DENIED");
            break;
        }
        case doip::PT_DiagMessage: {
            doip::DiagMessage dm;
            if (!doip::parse_diag_message(m.payload, dm)) break;
            if (!activated) {   // a tester must routing-activate first
                std::fprintf(stderr, "[doip_server] diag-message before activation — ignoring\n");
                break;
            }
            // ACK the receipt, then route the UDS to UdsRouter + return the reply.
            send_all(cfd, doip::make_diag_ack(kEcuAddress, dm.source_addr));
            doip::Bytes resp;
            if (call_uds_router(dm.uds, dm.source_addr, dm.target_addr, resp) &&
                !resp.empty()) {
                send_all(cfd, doip::make_diag_message(kEcuAddress, dm.source_addr, resp));
            } else {
                // Router unreachable → UDS general reject (0x7F sid 0x10).
                doip::Bytes nrc = {0x7F, dm.uds.empty() ? uint8_t(0) : dm.uds[0], 0x10};
                send_all(cfd, doip::make_diag_message(kEcuAddress, dm.source_addr, nrc));
            }
            break;
        }
        default:
            // Alive-check + unknowns: ignore (a fuller impl answers alive-check).
            break;
        }
    }
    ::close(cfd);
}

}  // namespace

// do_start — connect the RemoteRef into UdsRouter (the reply pump) + bind the
// TCP listener on 13400. Best-effort: a failed UdsRouter connect still lets the
// socket come up (the first call retries); a failed bind leaves up=false so
// do_loop exits + the supervisor restarts us.
void DoipServer::do_start() {
    std::fprintf(stderr, "[%s] DoIP server starting\n", kNodeName);

    if (S().ref.connect_instance(0, /*timeout_ms=*/1500)) {
        S().mux.watch_remote_ref(S().ref);
        S().mux.start();
        S().ref_up = true;
    } else {
        std::fprintf(stderr, "[%s] UdsRouter not reachable yet — first call retries\n",
                     kNodeName);
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("[doip_server] socket"); return; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kDoipPort);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("[doip_server] bind :13400");
        ::close(fd);
        return;
    }
    if (::listen(fd, 4) != 0) { std::perror("[doip_server] listen"); ::close(fd); return; }
    S().listen_fd = fd;
    S().up.store(true);
    std::fprintf(stderr, "[%s] listening on 0.0.0.0:%u (DoIP)\n", kNodeName, kDoipPort);
}

// do_loop — accept tester connections (one at a time; a diagnostic session is
// inherently serial) until stop. accept() is interrupted by do_stop closing the
// listen fd.
void DoipServer::do_loop() {
    if (!S().up.load()) return;   // startup failed → supervisor restarts us
    while (!stop_requested()) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(S().listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) {
            if (stop_requested()) break;
            continue;   // EINTR / transient
        }
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "[%s] tester connected from %s\n", kNodeName, ip);
        serve_connection(cfd);
    }
    std::fprintf(stderr, "[%s] loop exiting\n", kNodeName);
}

// do_stop — close the listener (wakes accept) + the RemoteRef pump.
void DoipServer::do_stop() {
    std::fprintf(stderr, "[%s] stopping\n", kNodeName);
    S().up.store(false);
    if (S().listen_fd >= 0) { ::close(S().listen_fd); S().listen_fd = -1; }
    if (S().ref_up) { S().mux.stop(); S().ref_up = false; }
}

}  // namespace ara::diag
