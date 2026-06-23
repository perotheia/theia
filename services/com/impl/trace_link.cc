// trace_link implementation — com's PG member for log[trace]'s TraceRecord group.
//
// MIGRATED to PG: com no longer Subscribes to TraceCtl (gone) over a SEQPACKET
// socket. It pg_joins the TraceRecord group — the supervisor allocates com's
// delivery {type, instance}, and TraceStreamPump PG-multicasts every record to
// it (and every other joiner). PgClient::join_raw hands each record's RAW
// proto-wire bytes to the sink; TraceForwarder_handlers.cc parses them into a
// libprotobuf services.com.TraceRecord and fans out to gRPC subscribers.
//
// The nanopb world is now just PgClient (hand-framed PgJoin CALL, no log proto
// at all), so trace_link.hpp's primitive surface stays free of nanopb headers.

#include "impl/trace_link.hpp"

#include "PgClient.hh"          // PG join + name-sequence multicast receive
#include "RemoteCodec.hh"       // RemoteCodec / msg_type_name for the group
#include "system/services/log/log.pb.h"   // system_services_log_TraceRecord_fields
#include "lib/log_codecs.hh"              // RemoteCodec<system_services_log_TraceRecord>

#include <atomic>
#include <cstdio>
#include <string>

namespace services_com {

// The TraceRecord wire type — its NAME is the PG group identity (the same
// msg_type_name<T>() the C++ pump broadcasts to).
using TraceRecord = system_services_log_TraceRecord;

struct TraceLink::Impl {
    ::theia::runtime::PgClient pg;
    TraceSink                  sink;
    std::atomic<bool>          running{false};
};

TraceLink::TraceLink() : impl_(new Impl()) {}
TraceLink::~TraceLink() { stop(); delete impl_; }

TraceLink& TraceLink::instance() {
    static TraceLink s;
    return s;
}

void TraceLink::set_sink(TraceSink sink) { impl_->sink = std::move(sink); }

bool TraceLink::start(int /*connect_timeout_ms*/) {
    if (impl_->running.load()) return true;

    impl_->pg.attach("com-trace-link", /*binding=*/nullptr);
    auto g = impl_->pg.join_raw<TraceRecord>(
        [this](const uint8_t* p, uint16_t len) {
            if (impl_->sink)
                impl_->sink(std::string(reinterpret_cast<const char*>(p), len));
        });
    if (!g.ok) {
        std::fprintf(stderr,
            "[trace_link] pg_join of the TraceRecord group failed "
            "(supervisor down?); no trace egress\n");
        return false;
    }
    impl_->running.store(true);
    std::fprintf(stderr,
        "[trace_link] pg_joined TraceRecord group (type=0x%08x inst=%u)\n",
        g.type, g.instance);
    return true;
}

void TraceLink::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->pg.shutdown();   // leave the group + stop the recv thread
}

bool TraceLink::running() const { return impl_->running.load(); }

}  // namespace services_com
