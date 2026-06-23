// log_link implementation — com's PG member for log[logging]'s LogRecord group.
//
// MIGRATED to PG (symmetric with trace_link/shwa_link): com no longer Subscribes
// to LogDaemon over SEQPACKET. It pg_joins the LogRecord group by the wire type
// NAME (string-keyed, so the nanopb log proto stays off this TU); the LogStreamPump
// pg_watch'es the group and casts each line to every member. PgClient::join_raw_named
// hands each record's RAW bytes to the sink; LogForwarder_handlers.cc parses them
// into a libprotobuf services.com.LogRecord and fans out to gRPC subscribers.

#include "impl/log_link.hpp"

#include "PgClient.hh"      // PG join_raw_named + name-sequence multicast receive

#include <atomic>
#include <cstdio>
#include <string>

namespace services_com {

namespace {
// The wire type name = the PG group identity (== LogStreamPump's
// msg_type_name<LogRecord>()). String-keyed → no nanopb log proto on this TU.
constexpr const char* kLogGroup = "system_services_log_LogRecord";
}  // namespace

struct LogLink::Impl {
    ::theia::runtime::PgClient pg;
    LogSink                    sink;
    std::atomic<bool>          running{false};
};

LogLink::LogLink() : impl_(new Impl()) {}
LogLink::~LogLink() { stop(); delete impl_; }

LogLink& LogLink::instance() {
    static LogLink s;
    return s;
}

void LogLink::set_sink(LogSink sink) { impl_->sink = std::move(sink); }

bool LogLink::start(int /*connect_timeout_ms*/) {
    if (impl_->running.load()) return true;
    impl_->pg.attach("com-log-link", /*binding=*/nullptr);
    auto g = impl_->pg.join_raw_named(kLogGroup,
        [this](const uint8_t* p, uint16_t len) {
            if (impl_->sink)
                impl_->sink(std::string(reinterpret_cast<const char*>(p), len));
        });
    if (!g.ok) {
        std::fprintf(stderr,
            "[log_link] pg_join of the LogRecord group failed "
            "(supervisor down?); no log egress\n");
        return false;
    }
    impl_->running.store(true);
    std::fprintf(stderr,
        "[log_link] pg_joined LogRecord group (type=0x%08x inst=%u)\n",
        g.type, g.instance);
    return true;
}

void LogLink::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->pg.shutdown();
}

bool LogLink::running() const { return impl_->running.load(); }

}  // namespace services_com
