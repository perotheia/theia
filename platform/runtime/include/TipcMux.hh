// TipcMux — per-process TIPC inbound multiplexer.
//
// Responsibilities:
//
//   * For each local node, bind a listening TIPC service at the node's
//     (tipc_type, tipc_instance) declared in its .art.
//   * Single epoll thread accepts new client connections, recvs inbound
//     SEQPACKET datagrams, and routes them by destination fd → node.
//   * For each destination node, maintain a tiny dispatch table mapping
//     service_id (msg-type hash) → {decode-into-typed-T, call
//     handle_cast/handle_call on that node}. Hand-registered at startup
//     via register_cast<T> / register_call<Req,Reply>; later codegen
//     emits this table.
//
// Reply-side demux (::theia::runtime::kMsgGenCallReply frames travelling back to
// THIS process from outbound calls we made via RemoteRef::send_request_)
// is handled separately — see RemoteRef::on_reply_(). The mux delegates
// to the appropriate RemoteRef via a reply-fd → RemoteRef* table.
//
// This is the multi-node generalization of a per-node TIPC server.

#pragma once

#include "GenServer.hh"
#include "NodeRef.hh"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// Templated register_call lambdas use ::send(MSG_NOSIGNAL) directly,
// so the socket headers have to be visible at the *header* point where
// users (p*_main.cc) instantiate the template.
#include <sys/socket.h>

#include "TheiaMsgHeader.hh"   // theia::runtime::TheiaMsgHeader + msg-type consts

namespace theia {
namespace runtime {

// One entry in a node's inbound dispatch table.
//
// Given the wire payload bytes, decode them into a typed T and call
// the right handler on the owning node. Closures capture the typed
// info; the registry sees only this opaque function pointer.
//
// For a CALL, the closure also encodes the typed reply and sends a
// ::theia::runtime::kMsgGenCallReply back on the originating fd. The mux passes the
// fd + correlation_id so the closure can build the reply frame.
struct InboundEntry {
    enum class Kind { Cast, Call };
    Kind kind;
    std::function<void(const uint8_t* payload, uint16_t len,
                        int reply_fd, uint32_t corr_id)> dispatch;
};

// Bookkeeping for one local node bound to TIPC.
struct NodeBinding {
    GenServerBase* node;
    int            listen_fd;
    uint32_t       tipc_type;
    uint32_t       tipc_instance;
    // service_id (msg-type hash) → dispatch entry.
    std::unordered_map<uint16_t, InboundEntry> entries;
};

class TipcMux {
public:
    TipcMux();
    ~TipcMux();
    TipcMux(const TipcMux&) = delete;
    TipcMux& operator=(const TipcMux&) = delete;

    // Bind a listening socket for `node` at the given TIPC address.
    // Returns a NodeBinding pointer that the caller uses to register
    // per-message-type dispatch entries.
    NodeBinding* bind_node(GenServerBase& node,
                            uint32_t tipc_type, uint32_t tipc_instance);

    // Bind a listening socket WITHOUT a GenServerBase — for a GenRunnable
    // (no mailbox). The returned binding's `node` is nullptr; only the
    // inline dispatch path (register_cast_inline) may be registered on it,
    // never the mailbox-enqueuing register_cast/register_call. Used so a
    // reporting runnable can receive the supervisor's LogLevelPush /
    // TraceControlPush and apply them on the mux thread.
    NodeBinding* bind_listener(uint32_t tipc_type, uint32_t tipc_instance);

    // Look up an already-bound node's binding by its TIPC address. Lets a
    // runnable's do_start() install extra dispatch entries on ANOTHER node's
    // binding (e.g. com's ComGrpcProxy registering the firehose casts on
    // ComDaemon's binding) without threading the pointer through main.cc.
    // Returns nullptr if no node is bound at (type, instance).
    NodeBinding* binding_for(uint32_t tipc_type, uint32_t tipc_instance);

    // Register a remote handle_cast handler for typed message Msg on
    // the given binding. Sender stamps service_id = djb2_low16("Msg");
    // receiver's `node` exposes handle_cast(const Msg&, State&).
    //
    // `conflate` (from a `[conflate]` .art port attr, gen-fc-emitted): a
    // periodic STATE-LIKE feed where only the newest matters. The enqueue routes
    // through enqueue_conflated(service_id, …) so a cast whose predecessor is
    // still queued OVERWRITES it in place — a slow consumer sees the latest, not
    // the stale backlog (docs/tasks genserver-conflating-mailbox). The
    // service_id IS the conflation key: one slot per message type.
    template <typename Msg, typename NodeT>
    void register_cast(NodeBinding* b, NodeT& node, bool conflate = false) {
        // Record the conflatable type on the node so the LOCAL same-process
        // cast() path keep-latests it too, not just this mux/TIPC path. Thread
        // the node's name in too, so a keep-latest drop can emit a Conflate
        // Tracer event (GenServerBase can't reach Derived::kNodeName itself).
        if (conflate) {
            node.mark_conflated(RemoteCodec<Msg>::service_id);
            node.set_trace_name(NodeT::kNodeName);
        }
        InboundEntry e;
        e.kind = InboundEntry::Kind::Cast;
        e.dispatch = [&node, conflate](const uint8_t* payload, uint16_t len,
                              int /*reply_fd*/, uint32_t corr) {
            // Inbound trace: Recv with the wire-level corr_id (which
            // is 0 for casts). Payload is the encoded request bytes
            // already in our hands — no extra pb_encode needed.
            auto& tr = ::theia::runtime::tracer_for(NodeT::kNodeName);
            if (tr.enabled()) {
                tr.emit(::theia::runtime::TraceEvent::Recv,
                        ::theia::runtime::msg_type_name<Msg>(),
                        corr, payload, len);
            }
            Msg msg{};
            pb_istream_t is = pb_istream_from_buffer(payload, len);
            if (!pb_decode(&is, RemoteCodec<Msg>::fields(), &msg)) return;
            auto handler = [msg = std::move(msg), corr](GenServerBase* base) {
                auto* self = static_cast<NodeT*>(base);
                auto& tr2 = ::theia::runtime::tracer_for(NodeT::kNodeName);
                if (tr2.enabled()) {
                    tr2.emit(::theia::runtime::TraceEvent::Dispatch,
                             ::theia::runtime::msg_type_name<Msg>(),
                             corr, nullptr, 0);
                }
                self->handle_cast(msg, self->state());
                if (tr2.enabled()) {
                    tr2.emit(::theia::runtime::TraceEvent::DispatchDone,
                             ::theia::runtime::msg_type_name<Msg>(),
                             corr, nullptr, 0);
                }
            };
            if (conflate)
                node.enqueue_conflated(RemoteCodec<Msg>::service_id,
                                       std::move(handler));
            else
                node.enqueue(std::move(handler));
        };
        b->entries[RemoteCodec<Msg>::service_id] = std::move(e);
    }

    // Register an INLINE cast handler for typed Msg — for a GenRunnable, which
    // has no mailbox to enqueue onto. The mux's dispatch thread decodes Msg and
    // calls (node.*method)(msg) DIRECTLY (no node thread, no enqueue). Use only
    // for handlers whose body is thread-safe off the node thread — the control
    // pushes (on_log_level_push / on_trace_control_push) qualify, as they touch
    // only atomics. service_id keys on the same djb2(Msg) hash as register_cast,
    // so the supervisor's push matches by construction.
    // MethodHost is separate from NodeT because the framework control
    // handlers (on_log_level_push / on_trace_control_push) live on the
    // GenRunnable<Derived> BASE, so &Derived::on_log_level_push has type
    // `void (GenRunnable<Derived>::*)(...)`, not `void (Derived::*)(...)`.
    // Deducing the host independently lets the call bind either.
    template <typename Msg, typename NodeT, typename MethodHost>
    void register_cast_inline(NodeBinding* b, NodeT& node,
                              void (MethodHost::*method)(const Msg&) noexcept) {
        InboundEntry e;
        e.kind = InboundEntry::Kind::Cast;
        e.dispatch = [&node, method](const uint8_t* payload, uint16_t len,
                                     int /*reply_fd*/, uint32_t corr) {
            auto& tr = ::theia::runtime::tracer_for(NodeT::kNodeName);
            if (tr.enabled()) {
                tr.emit(::theia::runtime::TraceEvent::Recv,
                        ::theia::runtime::msg_type_name<Msg>(),
                        corr, payload, len);
            }
            Msg msg{};
            pb_istream_t is = pb_istream_from_buffer(payload, len);
            if (!pb_decode(&is, RemoteCodec<Msg>::fields(), &msg)) return;
            (node.*method)(msg);   // inline on the mux thread — no mailbox
        };
        b->entries[RemoteCodec<Msg>::service_id] = std::move(e);
    }

    // Register a remote handle_call handler for typed (Req → Reply).
    // The node thread runs handle_call(req, state) and returns Reply by
    // value; the mux's dispatcher encodes it and sends a CALL_REPLY
    // frame back on the originating client fd, with correlation_id
    // echoed verbatim.
    template <typename Req, typename Reply, typename NodeT>
    void register_call(NodeBinding* b, NodeT& node) {
        InboundEntry e;
        e.kind = InboundEntry::Kind::Call;
        e.dispatch = [&node](const uint8_t* payload, uint16_t len,
                              int reply_fd, uint32_t corr) {
            // Inbound trace: Recv on the mux thread (with corr from
            // the wire header — paired with the sender's Send and the
            // eventual SendReply).
            auto& tr = ::theia::runtime::tracer_for(NodeT::kNodeName);
            if (tr.enabled()) {
                tr.emit(::theia::runtime::TraceEvent::Recv,
                        ::theia::runtime::msg_type_name<Req>(),
                        corr, payload, len);
            }
            Req req{};
            pb_istream_t is = pb_istream_from_buffer(payload, len);
            if (!pb_decode(&is, RemoteCodec<Req>::fields(), &req)) return;

            node.enqueue([req = std::move(req), reply_fd, corr](
                              GenServerBase* base) mutable {
                auto* self = static_cast<NodeT*>(base);
                auto& tr2 = ::theia::runtime::tracer_for(NodeT::kNodeName);
                if (tr2.enabled()) {
                    tr2.emit(::theia::runtime::TraceEvent::Dispatch,
                             ::theia::runtime::msg_type_name<Req>(),
                             corr, nullptr, 0);
                }
                Reply reply = self->handle_call(req, self->state());
                if (tr2.enabled()) {
                    tr2.emit(::theia::runtime::TraceEvent::DispatchDone,
                             ::theia::runtime::msg_type_name<Req>(),
                             corr, nullptr, 0);
                }

                // Reply encode buffer. Sized for the LARGEST reply any op
                // returns — the supervisor's TreeSnapshot (ChildState rows at
                // ~418 B worst case) is the driver: 256 (the original cap)
                // silently dropped it (pb_encode overflowed → `return` → no
                // reply → the caller timed out), 48 KB covered the old 64-row
                // cap, and 64 KB covers the current 128-row cap (~52 KB).
                // proto_len is u16 and is assigned by a NARROWING cast below,
                // so the cap must stay <= 65535: at exactly 64 KB a full buffer
                // would wrap proto_len to 0 and ship a silently corrupt frame.
                // 65535 is the HARD ceiling — a tree past ~160 rows cannot ride
                // one reply at all, and GetTree must paginate (or move to the
                // NodeEdge/NodeState firehose) rather than grow this further.
                // Heap, not stack (too big to stack per call).
                static constexpr size_t kReplyCap = 64 * 1024 - 1;
                std::vector<uint8_t> buf(kReplyCap);
                pb_ostream_t os = pb_ostream_from_buffer(buf.data(), kReplyCap);
                if (!pb_encode(&os, RemoteCodec<Reply>::fields(), &reply))
                    return;

                if (tr2.enabled()) {
                    tr2.emit(::theia::runtime::TraceEvent::SendReply,
                             ::theia::runtime::msg_type_name<Reply>(),
                             corr, buf.data(), (uint16_t)os.bytes_written);
                }

                TheiaMsgHeader rh{};
                rh.bus_type            = ::theia::runtime::kBusTypeRpc;
                rh.msg_type            = ::theia::runtime::kMsgGenCallReply;
                // proto_len is uint16_t: an oversize reply (>65535 B) would wrap
                // to a bogus/zero length and ship a corrupt frame. Refuse LOUDLY
                // instead — the client sees a missing reply (call timeout) rather
                // than a silently-truncated one. (GetTree's TreeSnapshot is the
                // real oversize risk on a big multi-board tree.)
                if (!::theia::runtime::narrow_proto_len(os.bytes_written,
                                                        rh.proto_len)) {
                    std::fprintf(stderr,
                        "[tipcmux] reply for service 0x%04x is %zu B > 65535 — "
                        "refusing to send a would-wrap frame (call will time "
                        "out; paginate the reply)\n",
                        (unsigned)RemoteCodec<Reply>::service_id,
                        (size_t)os.bytes_written);
                    return;
                }
                rh.rpc.service_id      = RemoteCodec<Reply>::service_id;
                rh.rpc.method_id       = 0;
                rh.rpc.correlation_id  = corr;
                std::vector<uint8_t> framebuf(sizeof(TheiaMsgHeader)
                                              + os.bytes_written);
                std::memcpy(framebuf.data(), &rh, sizeof(TheiaMsgHeader));
                std::memcpy(framebuf.data() + sizeof(TheiaMsgHeader),
                            buf.data(), os.bytes_written);
                ::send(reply_fd, framebuf.data(),
                       sizeof(TheiaMsgHeader) + os.bytes_written,
                       MSG_NOSIGNAL);
            });
        };
        b->entries[RemoteCodec<Req>::service_id] = std::move(e);
    }

    // Tell the mux about a RemoteRef whose outbound TIPC client is
    // expecting reply frames. The mux watches the client fd's read
    // side and routes incoming ::theia::runtime::kMsgGenCallReply frames to the
    // RemoteRef via its on_reply_() member.
    template <typename T, uint32_t TT, uint32_t TI>
    void watch_remote_ref(RemoteRef<T, TT, TI>& ref) {
        watch_fd_for_replies_(ref.client().fd(),
            [&ref](uint32_t corr, const uint8_t* data, uint16_t len) {
                ref.on_reply_(corr, data, len);
            });
    }

    // Public reply-fd registration for an ad-hoc RemoteRef (created in a
    // handler, not at startup). Adds the fd to this mux's epoll loop and
    // routes its call-replies to `sink`. Used by the free watch_reply_fd()
    // below via process_mux().
    void watch_reply_fd(
        int fd,
        std::function<void(uint32_t, const uint8_t*, uint16_t)> sink) {
        watch_fd_for_replies_(fd, std::move(sink));
    }

    // Unregister a reply fd watched via watch_reply_fd() — drop it from epoll
    // and from reply_sinks_. A RemoteRef MUST call this in its destructor,
    // BEFORE it closes the socket, or the entry (and the epoll registration)
    // leaks: per-call ad-hoc RemoteRefs (a handler doing call() on a fresh ref
    // each invocation) otherwise pile up hundreds of stale sinks and spin the
    // loop on the reused-then-closed fds. (Defined in TipcMux.cc — epoll there.)
    void unwatch_reply_fd(int fd);

    void start();
    void stop();

private:
    void loop_();
    int  bind_listen_(uint32_t type, uint32_t instance);
    void add_to_epoll_(int fd, uint32_t event_mask);
    void watch_fd_for_replies_(int fd,
        std::function<void(uint32_t, const uint8_t*, uint16_t)> sink);

    std::atomic<bool>               running_{false};
    std::thread                     thread_;
    int                             epoll_fd_ = -1;
    std::mutex                      mu_;
    std::vector<std::unique_ptr<NodeBinding>>  bindings_;
    std::unordered_map<int, NodeBinding*>      listen_fd_to_binding_;
    // accepted client fd → binding it speaks to (for inbound dispatch).
    std::unordered_map<int, NodeBinding*>      client_fd_to_binding_;
    // outbound client fd → reply sink (for our own RemoteRef replies).
    std::unordered_map<int,
        std::function<void(uint32_t, const uint8_t*, uint16_t)>> reply_sinks_;
};

// ---- process-wide TipcMux accessor ---------------------------------------
//
// There is ONE TipcMux (epoll/select loop) per app process — it accepts
// inbound node connections AND demuxes our own RemoteRef call-replies.
// Publishing it here (mirrors process_logger / process_timers) lets a
// handler's ad-hoc RemoteRef register its reply fd with the single loop
// via process_mux()->watch_remote_ref(ref), so a synchronous call(ref,...)
// made from inside a handler actually gets its reply pumped. main sets
// this once, before nodes start. Non-owning pointer (main owns the mux).
void      set_process_mux(TipcMux* mux) noexcept;
TipcMux*  process_mux() noexcept;   // nullptr if unset (caller checks)

}  // namespace runtime
}  // namespace theia
