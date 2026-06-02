// LocalRef<T> / RemoteRef<T, tipc_type, tipc_instance>.
//
// Two flavors of "handle to a gen_server node":
//
//   LocalRef<T>                          — wraps a T& in this process.
//                                          cast/call use the in-process
//                                          mailbox (existing path).
//
//   RemoteRef<T, type, instance>         — wraps a connected TIPC client
//                                          to a peer process hosting T.
//                                          cast/call nanopb-encode the
//                                          message and send a
//                                          ::theia::runtime::kMsgGenCast / _CALL frame.
//                                          The reply (for call) demuxes
//                                          on correlation_id, same as
//                                          the Status RPC we already
//                                          validated against cmp_gw.
//
// User code is identical for both. The cast() / call() free functions
// are overloaded on the ref type and pick the right path at compile
// time. The .art composition's `on process P` annotation drives which
// kind of ref the generator emits in the per-process routing header.
//
// Serialization: every remote message type T has a small "RemoteCodec<T>"
// trait giving its (service_id, *_fields descriptor). Today written by
// hand (one block per type the demo uses); later artheia codegen emits
// it.

#pragma once

#include "GenServer.hh"
#include "RemoteCodec.hh"        // RemoteCodec<T>, msg_type_name<T>,
                                  // encode_for_trace<T> — all live here
                                  // so GenServer.hh's trace hooks can
                                  // see them too (NodeRef.hh would be
                                  // circular).

#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <pb_decode.h>

namespace theia {
namespace runtime {


// ---- LocalRef<T> --------------------------------------------------------

template <typename T>
class LocalRef {
public:
    LocalRef() = default;
    explicit LocalRef(T& target) : target_(&target) {}

    // True if a target has been wired.
    bool valid() const noexcept { return target_ != nullptr; }

    T& target() noexcept { return *target_; }

    // Used by cast/call overloads — exposes the underlying GenServer
    // for the existing pass-by-value path.
    T* operator->() noexcept { return target_; }

private:
    T* target_ = nullptr;
};


// ---- TipcClient (raw connect + send + recv) -----------------------------

// One connected client socket targeting a specific (type, instance).
// Owns the fd. recv_one() reads one SEQPACKET datagram into a buffer.
// Used by RemoteRef and by the response-demuxer.
class TipcClient {
public:
    TipcClient() = default;
    ~TipcClient();
    TipcClient(const TipcClient&) = delete;
    TipcClient& operator=(const TipcClient&) = delete;

    // Eager-with-retry: try for up to `total_timeout_ms` in `retry_ms`
    // bursts. Returns true on first successful connect.
    bool connect(uint32_t tipc_type, uint32_t tipc_instance,
                  int total_timeout_ms = 3000,
                  int retry_ms = 100);

    void disconnect();
    bool is_open() const noexcept { return fd_ >= 0; }
    int  fd() const noexcept { return fd_; }

    // Send a full frame: 24-byte header + proto_len bytes of payload.
    bool send_frame(const TheiaMsgHeader& hdr,
                     const uint8_t* payload, uint16_t proto_len);

private:
    int fd_ = -1;
};


// Register a RemoteRef's reply fd with the ONE process TipcMux (the
// per-app epoll loop) so its call-replies get demuxed there. Type-erased
// (fd + sink) to avoid a NodeRef<->TipcMux include cycle — defined in
// TipcMux.cc, a no-op when no process mux was published. Called from
// RemoteRef::connect() so a synchronous call(ref,...) from inside a
// handler actually gets pumped.
void watch_reply_fd(
    int fd,
    std::function<void(uint32_t /*corr*/, const uint8_t* /*data*/,
                       uint16_t /*len*/)> sink);

// Counterpart to watch_reply_fd — drop the fd from the process mux's reply
// demux (epoll + reply_sinks_). Called from ~RemoteRef BEFORE the socket
// closes, so a per-call ad-hoc RemoteRef doesn't leak a sink + epoll entry
// every invocation. No-op when no process mux was published.
void unwatch_reply_fd(int fd);


// ---- RemoteRef<T, tipc_type, tipc_instance> ----------------------------
//
// One shared TipcClient + reply-demux state per RemoteRef. The first
// cast/call lazily connects; subsequent calls reuse the connection.
// RPC reply demux uses correlation_id (same pattern as GwClient).

template <typename NodeType, uint32_t TipcType, uint32_t TipcInstance>
class RemoteRef {
public:
    static constexpr uint32_t tipc_type     = TipcType;
    static constexpr uint32_t tipc_instance = TipcInstance;

    RemoteRef() = default;

    // Unregister this ref's reply fd from the process mux BEFORE ~TipcClient
    // closes the socket — otherwise each per-call ad-hoc RemoteRef leaks its
    // sink + epoll entry (the driver→counter Get-per-tick path piled up
    // hundreds of stale fds and spun the loop). Mirrors connect()'s
    // watch_reply_fd.
    ~RemoteRef() {
        if (watched_fd_ >= 0) {
            unwatch_reply_fd(watched_fd_);
            watched_fd_ = -1;
        }
    }

    RemoteRef(const RemoteRef&) = delete;
    RemoteRef& operator=(const RemoteRef&) = delete;

    // Eager connect; must be called once before first cast/call. Also
    // registers this ref's reply fd with the process TipcMux so a
    // synchronous call() from inside a handler gets its reply pumped by
    // the one per-app epoll loop (no-op if no process mux is published —
    // e.g. a cast-only caller, or a test without a mux).
    bool connect(int timeout_ms = 3000) {
        if (!client_.connect(TipcType, TipcInstance, timeout_ms))
            return false;
        watched_fd_ = client_.fd();
        watch_reply_fd(watched_fd_,
            [this](uint32_t corr, const uint8_t* data, uint16_t len) {
                this->on_reply_(corr, data, len);
            });
        return true;
    }

    // Sends a ::theia::runtime::kMsgGenCast frame carrying Msg.
    // Trace: tagged with the TARGET node's kNodeName (NodeType::kNodeName)
    // so the trace stream stays symmetric with the inbound Recv (also
    // tagged with the target's name). corr_id is the wire-level value,
    // which for casts is 0 — collector matches Send/Recv via
    // (msg_type, payload_hash) for casts.
    template <typename Msg>
    bool cast_(const Msg& msg) {
        using Codec = RemoteCodec<Msg>;
        uint8_t buf[256];
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&os, Codec::fields(), &msg)) return false;
        auto& tr = ::theia::runtime::tracer_for(NodeType::kNodeName);
        if (tr.enabled()) {
            tr.emit(::theia::runtime::TraceEvent::Send,
                    ::theia::runtime::msg_type_name<Msg>(),
                    /*corr_id=*/0,
                    buf, (uint16_t)os.bytes_written,
                    /*dst=*/NodeType::kNodeName);  // the peer this Send targets
        }
        TheiaMsgHeader hdr{};
        hdr.bus_type            = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type            = ::theia::runtime::kMsgGenCast;
        hdr.proto_len           = (uint16_t)os.bytes_written;
        hdr.rpc.service_id      = Codec::service_id;
        hdr.rpc.method_id       = 0;
        hdr.rpc.correlation_id  = 0;  // cast has no reply
        return client_.send_frame(hdr, buf, (uint16_t)os.bytes_written);
    }

    // Sends a ::theia::runtime::kMsgGenCall frame, returns a future for the reply.
    // The reply demuxer (run by the caller's TipcMux on inbound) wakes
    // this future when the matching correlation_id arrives.
    template <typename Reply, typename Req>
    std::future<Reply> send_request_(const Req& req) {
        using Codec = RemoteCodec<Req>;
        auto promise = std::make_shared<std::promise<Reply>>();
        auto fut = promise->get_future();

        uint32_t corr;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            corr = next_corr_++;
            pending_[corr] = [promise](const uint8_t* data, uint16_t len) {
                Reply r{};
                pb_istream_t is = pb_istream_from_buffer(data, len);
                if (pb_decode(&is, RemoteCodec<Reply>::fields(), &r)) {
                    promise->set_value(std::move(r));
                } else {
                    promise->set_exception(std::make_exception_ptr(
                        std::runtime_error("pb_decode failed in reply")));
                }
            };
        }

        uint8_t buf[256];
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        if (!pb_encode(&os, Codec::fields(), &req)) {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.erase(corr);
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("pb_encode failed")));
            return fut;
        }
        TheiaMsgHeader hdr{};
        hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type           = ::theia::runtime::kMsgGenCall;
        hdr.proto_len          = (uint16_t)os.bytes_written;
        hdr.rpc.service_id     = Codec::service_id;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = corr;
        // Trace the outbound CALL request. corr is the wire-level id;
        // the matching Recv on the server side and the CallResult on
        // this side will both carry it.
        auto& tr = ::theia::runtime::tracer_for(NodeType::kNodeName);
        if (tr.enabled()) {
            tr.emit(::theia::runtime::TraceEvent::Send,
                    ::theia::runtime::msg_type_name<Req>(),
                    corr, buf, (uint16_t)os.bytes_written,
                    /*dst=*/NodeType::kNodeName);  // the peer this CALL targets
        }
        if (!client_.send_frame(hdr, buf, (uint16_t)os.bytes_written)) {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.erase(corr);
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("TIPC send failed")));
        }
        return fut;
    }

    // Called by the per-process TipcMux when a ::theia::runtime::kMsgGenCallReply
    // arrives on this RemoteRef's client fd. Looks up the pending
    // promise by correlation_id and fulfills it.
    void on_reply_(uint32_t corr, const uint8_t* data, uint16_t len) {
        // Trace the reply arrival before we hand off to the demux. The
        // caller (whoever's holding the future) sees this as the
        // moment "the reply landed on the wire". Paired with the
        // SendReply event emitted by the server-side TipcMux.
        auto& tr = ::theia::runtime::tracer_for(NodeType::kNodeName);
        if (tr.enabled()) {
            tr.emit(::theia::runtime::TraceEvent::CallResult,
                    /*msg_type_name=*/"reply",
                    corr, data, len);
        }
        std::function<void(const uint8_t*, uint16_t)> fn;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            auto it = pending_.find(corr);
            if (it == pending_.end()) return;
            fn = std::move(it->second);
            pending_.erase(it);
        }
        fn(data, len);
    }

    TipcClient& client() { return client_; }

private:
    TipcClient client_;
    int        watched_fd_{-1};  // fd registered with the process mux (-1 = none)
    uint32_t   next_corr_{1};
    std::mutex pending_mu_;
    std::unordered_map<uint32_t,
        std::function<void(const uint8_t*, uint16_t)>> pending_;
};


// ---- TipcAddr — runtime TIPC address pair --------------------------------
//
// The compile-time-templated RemoteRef<NodeType, TT, TI> hardcodes the
// destination in the type. Netgraph LUTs need the dual: address is a
// constexpr VALUE that the generated <Node>_netgraph.hh emits per
// reachable peer, and `cast(Daemon&, Msg, TipcAddr)` looks it up at
// runtime.
//
// This is the user-facing primitive after the netgraph redesign —
// app code writes `cast(*this, msg, netgraph::exec)` where
// `netgraph::exec` is a `constexpr TipcAddr` from the gen-app-emitted
// header. The runtime builds a per-call TipcClient + frame-sends.
//
// Cost: each cast opens a fresh TIPC connection. For high-frequency
// signals a per-peer connection cache could help — not in this round.
struct TipcAddr {
    uint32_t type;
    uint32_t instance;
    // Peer node identity (kNodeName) for trace `dst`. Optional — older
    // netgraph headers emit {type, instance} only, leaving this nullptr, in
    // which case the Send's dst is "". The generator now fills it with the
    // peer's prototype name so addressed casts carry their destination.
    const char* name = nullptr;
};


// ---- cast / call / send_request overloads -------------------------------

// Local path: dispatch into the existing mailbox lambda.
template <typename T, typename Msg>
void cast(LocalRef<T>& ref, Msg msg) {
    cast(ref.target(), std::move(msg));
}

// ---- Netgraph LUT path ---------------------------------------------------
//
// `cast(daemon, msg, TipcAddr)` — addressed by a runtime constexpr
// from the netgraph header. The `daemon` arg is only used for the
// trace tag (`Daemon::kNodeName`) so the trace stream stays
// consistent with the existing RemoteRef path.
template <typename Daemon, typename Msg>
void cast(Daemon& /*self*/, const Msg& msg, TipcAddr addr,
          const char* dst_name = nullptr) {
    using Codec = RemoteCodec<Msg>;
    // Ad-hoc client — one fresh connection per cast. Fine for low-
    // frequency signals (state broadcasts). A per-peer client cache
    // is the optimization step, kept for a future round.
    TipcClient client;
    if (!client.connect(addr.type, addr.instance)) return;

    uint8_t buf[256];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, Codec::fields(), &msg)) return;

    // Here Daemon::kNodeName is the SENDER (self) — the correct src for this
    // netgraph-addressed path — and the peer name comes from the TipcAddr the
    // netgraph LUT resolved (addr.name = the peer's kNodeName); an explicit
    // dst_name arg overrides it. nullptr → dst "".
    const char* dst = dst_name ? dst_name : addr.name;
    auto& tr = ::theia::runtime::tracer_for(Daemon::kNodeName);
    if (tr.enabled()) {
        tr.emit(::theia::runtime::TraceEvent::Send,
                ::theia::runtime::msg_type_name<Msg>(),
                /*corr_id=*/0,
                buf, (uint16_t)os.bytes_written,
                /*dst=*/dst);
    }

    TheiaMsgHeader hdr{};
    hdr.bus_type            = ::theia::runtime::kBusTypeRpc;
    hdr.msg_type            = ::theia::runtime::kMsgGenCast;
    hdr.proto_len           = (uint16_t)os.bytes_written;
    hdr.rpc.service_id      = Codec::service_id;
    hdr.rpc.method_id       = 0;
    hdr.rpc.correlation_id  = 0;
    client.send_frame(hdr, buf, (uint16_t)os.bytes_written);
    // Connection drops on `client` going out of scope. Future
    // per-peer caching would pin them.
}

template <typename Reply, typename T, typename Req, typename Act>
CallResult<Reply, Act> call(LocalRef<T>& ref, Req req, Act act,
                              int timeout_ms) {
    return call<Reply>(ref.target(), std::move(req), std::move(act),
                        timeout_ms);
}

template <typename Reply, typename T, typename Req, typename Act>
RequestId<Reply, Act> send_request(LocalRef<T>& ref, Req req, Act act) {
    return send_request<Reply>(ref.target(), std::move(req), std::move(act));
}

// Remote path: serialize and dispatch over TIPC. Same call signatures.
template <typename T, uint32_t TT, uint32_t TI, typename Msg>
void cast(RemoteRef<T, TT, TI>& ref, Msg msg) {
    ref.template cast_<Msg>(msg);
}

template <typename Reply, typename T, uint32_t TT, uint32_t TI,
          typename Req, typename Act>
CallResult<Reply, Act> call(RemoteRef<T, TT, TI>& ref, Req req, Act act,
                              int timeout_ms) {
    auto fut = ref.template send_request_<Reply, Req>(req);
    auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        return CallResult<Reply, Act>{
            CallTag::Timeout, std::move(act), Reply{}, {}};
    }
    try {
        Reply r = fut.get();
        return CallResult<Reply, Act>{
            CallTag::Reply, std::move(act), std::move(r), {}};
    } catch (const std::exception& e) {
        return CallResult<Reply, Act>{
            CallTag::Error, std::move(act), Reply{}, e.what()};
    }
}

// send_request over TIPC. Returns a RequestId<Reply, Act> compatible
// with the existing wait_response / check_response / RequestIdCollection
// surface. Internally bridges from RemoteRef::send_request_'s
// std::future<Reply> to the RequestId shape.
template <typename Reply, typename T, uint32_t TT, uint32_t TI,
          typename Req, typename Act>
RequestId<Reply, Act> send_request(RemoteRef<T, TT, TI>& ref,
                                     Req req, Act act) {
    auto fut = ref.template send_request_<Reply, Req>(req);
    return RequestId<Reply, Act>(std::move(fut), std::move(act));
}

}  // namespace runtime
}  // namespace theia
