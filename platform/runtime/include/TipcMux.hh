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
// Reply-side demux (GW_MSG_GEN_CALL_REPLY frames travelling back to
// THIS process from outbound calls we made via RemoteRef::send_request_)
// is handled separately — see RemoteRef::on_reply_(). The mux delegates
// to the appropriate RemoteRef via a reply-fd → RemoteRef* table.
//
// This is the multi-node generalization of libgw's GwTipcServer.

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

extern "C" {
#include "gw_proto.h"
}

namespace demo {
namespace runtime {

// One entry in a node's inbound dispatch table.
//
// Given the wire payload bytes, decode them into a typed T and call
// the right handler on the owning node. Closures capture the typed
// info; the registry sees only this opaque function pointer.
//
// For a CALL, the closure also encodes the typed reply and sends a
// GW_MSG_GEN_CALL_REPLY back on the originating fd. The mux passes the
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

    // Register a remote handle_cast handler for typed message Msg on
    // the given binding. Sender stamps service_id = djb2_low16("Msg");
    // receiver's `node` exposes handle_cast(const Msg&, State&).
    template <typename Msg, typename NodeT>
    void register_cast(NodeBinding* b, NodeT& node) {
        InboundEntry e;
        e.kind = InboundEntry::Kind::Cast;
        e.dispatch = [&node](const uint8_t* payload, uint16_t len,
                              int /*reply_fd*/, uint32_t corr) {
            // Inbound trace: Recv with the wire-level corr_id (which
            // is 0 for casts). Payload is the encoded request bytes
            // already in our hands — no extra pb_encode needed.
            auto& tr = ::demo::runtime::tracer_for(NodeT::kNodeName);
            if (tr.enabled()) {
                tr.emit(::demo::runtime::TraceEvent::Recv,
                        ::demo::runtime::msg_type_name<Msg>(),
                        corr, payload, len);
            }
            Msg msg{};
            pb_istream_t is = pb_istream_from_buffer(payload, len);
            if (!pb_decode(&is, RemoteCodec<Msg>::fields(), &msg)) return;
            node.enqueue([msg = std::move(msg), corr](GenServerBase* base) {
                auto* self = static_cast<NodeT*>(base);
                auto& tr2 = ::demo::runtime::tracer_for(NodeT::kNodeName);
                if (tr2.enabled()) {
                    tr2.emit(::demo::runtime::TraceEvent::Dispatch,
                             ::demo::runtime::msg_type_name<Msg>(),
                             corr, nullptr, 0);
                }
                self->handle_cast(msg, self->state());
                if (tr2.enabled()) {
                    tr2.emit(::demo::runtime::TraceEvent::DispatchDone,
                             ::demo::runtime::msg_type_name<Msg>(),
                             corr, nullptr, 0);
                }
            });
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
            auto& tr = ::demo::runtime::tracer_for(NodeT::kNodeName);
            if (tr.enabled()) {
                tr.emit(::demo::runtime::TraceEvent::Recv,
                        ::demo::runtime::msg_type_name<Req>(),
                        corr, payload, len);
            }
            Req req{};
            pb_istream_t is = pb_istream_from_buffer(payload, len);
            if (!pb_decode(&is, RemoteCodec<Req>::fields(), &req)) return;

            node.enqueue([req = std::move(req), reply_fd, corr](
                              GenServerBase* base) mutable {
                auto* self = static_cast<NodeT*>(base);
                auto& tr2 = ::demo::runtime::tracer_for(NodeT::kNodeName);
                if (tr2.enabled()) {
                    tr2.emit(::demo::runtime::TraceEvent::Dispatch,
                             ::demo::runtime::msg_type_name<Req>(),
                             corr, nullptr, 0);
                }
                Reply reply = self->handle_call(req, self->state());
                if (tr2.enabled()) {
                    tr2.emit(::demo::runtime::TraceEvent::DispatchDone,
                             ::demo::runtime::msg_type_name<Req>(),
                             corr, nullptr, 0);
                }

                uint8_t buf[256];
                pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
                if (!pb_encode(&os, RemoteCodec<Reply>::fields(), &reply))
                    return;

                if (tr2.enabled()) {
                    tr2.emit(::demo::runtime::TraceEvent::SendReply,
                             ::demo::runtime::msg_type_name<Reply>(),
                             corr, buf, (uint16_t)os.bytes_written);
                }

                GwMessageHeader rh{};
                rh.bus_type            = GW_BUS_TYPE_RPC;
                rh.msg_type            = GW_MSG_GEN_CALL_REPLY;
                rh.proto_len           = (uint16_t)os.bytes_written;
                rh.rpc.service_id      = RemoteCodec<Reply>::service_id;
                rh.rpc.method_id       = 0;
                rh.rpc.correlation_id  = corr;
                uint8_t framebuf[sizeof(GwMessageHeader) + 256];
                std::memcpy(framebuf, &rh, sizeof(GwMessageHeader));
                std::memcpy(framebuf + sizeof(GwMessageHeader), buf,
                            os.bytes_written);
                ::send(reply_fd, framebuf,
                       sizeof(GwMessageHeader) + os.bytes_written,
                       MSG_NOSIGNAL);
            });
        };
        b->entries[RemoteCodec<Req>::service_id] = std::move(e);
    }

    // Tell the mux about a RemoteRef whose outbound TIPC client is
    // expecting reply frames. The mux watches the client fd's read
    // side and routes incoming GW_MSG_GEN_CALL_REPLY frames to the
    // RemoteRef via its on_reply_() member.
    template <typename T, uint32_t TT, uint32_t TI>
    void watch_remote_ref(RemoteRef<T, TT, TI>& ref) {
        watch_fd_for_replies_(ref.client().fd(),
            [&ref](uint32_t corr, const uint8_t* data, uint16_t len) {
                ref.on_reply_(corr, data, len);
            });
    }

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

}  // namespace runtime
}  // namespace demo
