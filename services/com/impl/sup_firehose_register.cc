// register_firehose_casts — see sup_firehose_register.hpp.
//
// This TU owns the supervisor NANOPB structs + their RemoteCodec<>
// specializations (supervisor_codecs.hh). It installs custom Cast dispatch
// entries directly on the NodeBinding (the same map TipcMux::register_cast<>
// writes), each decoding a nanopb struct on the mux epoll thread and feeding
// SupFirehose with PLAIN SCALARS. We bypass ComDaemon::handle_cast (the
// generated header doesn't know these types, and SupervisorEventIf is an empty
// placeholder) — the reassembler IS the consumer.
//
// Why custom entries instead of register_cast<Msg, ComDaemon>: register_cast
// hard-codes node.handle_cast(msg, state) + a node-thread enqueue. ComDaemon
// has no such overload, and the firehose has no reason to hop to the node
// thread — SupFirehose is internally mutex-guarded.

#include "impl/sup_firehose_register.hpp"
#include "impl/sup_firehose.hpp"

#include "TipcMux.hh"        // NodeBinding + InboundEntry
#include "RemoteCodec.hh"

// nanopb supervisor structs + RemoteCodec specializations (service_ids).
#include "supervisor/supervisor_codecs.hh"

extern "C" {
#include <pb_decode.h>
}

namespace services_com {

namespace {

template <typename Msg, typename Sink>
void install(theia::runtime::NodeBinding* b, Sink sink) {
    theia::runtime::InboundEntry e;
    e.kind = theia::runtime::InboundEntry::Kind::Cast;
    e.dispatch = [sink](const uint8_t* payload, uint16_t len,
                        int /*reply_fd*/, uint32_t /*corr*/) {
        Msg msg{};
        pb_istream_t is = pb_istream_from_buffer(payload, len);
        if (!pb_decode(&is, theia::runtime::RemoteCodec<Msg>::fields(), &msg))
            return;
        sink(msg);
    };
    b->entries[theia::runtime::RemoteCodec<Msg>::service_id] = std::move(e);
}

}  // namespace

void register_firehose_casts(theia::runtime::NodeBinding* b) {
    if (!b) return;
    auto& fh = SupFirehose::instance();

    install<services_supervisor_SnapshotBegin>(
        b, [&fh](const services_supervisor_SnapshotBegin& m) {
            fh.on_snapshot_begin(m.generation, m.timestamp_ms);
        });
    install<services_supervisor_NodeEdge>(
        b, [&fh](const services_supervisor_NodeEdge& m) {
            fh.on_node_edge(m.op, m.parent_name, m.name, m.kind);
        });
    install<services_supervisor_NodeState>(
        b, [&fh](const services_supervisor_NodeState& m) {
            fh.on_node_state(m.name, m.pid, m.tid, m.state, m.flags,
                             m.restart_count, m.last_exit_code, m.uptime_ms,
                             m.cpu_pct, m.rss_kb, m.vsz_kb, m.threads,
                             m.shared_kb, m.data_kb);
        });
    install<services_supervisor_SnapshotEnd>(
        b, [&fh](const services_supervisor_SnapshotEnd& m) {
            fh.on_snapshot_end(m.generation);
        });
}

}  // namespace services_com
