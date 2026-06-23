// shwa_link implementation — com's PG member for SHWA's AccelSample group.
//
// MIGRATED to PG: SHWA's AccelSubmitter (a SOCK_DGRAM sendto to a fixed egress
// name 0x8001001A) is retired. SHWA now PG-multicasts AccelSample; com pg_joins
// that group — the supervisor allocates com's delivery address and the kernel
// fans out each sample. We join by the wire type NAME (string) so the nanopb
// shwa header stays off this TU; PgClient hands each sample's RAW bytes to the
// sink, and ComGrpcProxy re-parses them into a libprotobuf services.com.AccelSample.

#include "impl/shwa_link.hpp"

#include "PgClient.hh"      // PG join_raw_named + name-sequence multicast receive

#include <atomic>
#include <cstdio>
#include <string>

namespace services_com {

namespace {
// The wire type name = the PG group identity, identical to SHWA's
// msg_type_name<AccelSample>() (services/shwa). String-keyed so com keeps the
// nanopb shwa proto off this raw-bytes TU.
constexpr const char* kAccelGroup = "system_services_shwa_AccelSample";
}  // namespace

struct ShwaLink::Impl {
    ::theia::runtime::PgClient pg;
    AccelSink                  sink;
    std::atomic<bool>          running{false};
};

ShwaLink::ShwaLink() : impl_(new Impl()) {}
ShwaLink::~ShwaLink() { stop(); delete impl_; }

ShwaLink& ShwaLink::instance() {
    static ShwaLink s;
    return s;
}

void ShwaLink::set_sink(AccelSink sink) { impl_->sink = std::move(sink); }

bool ShwaLink::start() {
    if (impl_->running.load()) return true;
    impl_->pg.attach("com-shwa-link", /*binding=*/nullptr);
    auto g = impl_->pg.join_raw_named(kAccelGroup,
        [this](const uint8_t* p, uint16_t len) {
            if (impl_->sink)
                impl_->sink(std::string(reinterpret_cast<const char*>(p), len));
        });
    if (!g.ok) {
        std::fprintf(stderr,
            "[shwa_link] pg_join of the AccelSample group failed "
            "(supervisor down?); no telemetry egress\n");
        return false;
    }
    impl_->running.store(true);
    std::fprintf(stderr,
        "[shwa_link] pg_joined AccelSample group (type=0x%08x inst=%u)\n",
        g.type, g.instance);
    return true;
}

void ShwaLink::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->pg.shutdown();
}

bool ShwaLink::running() const { return impl_->running.load(); }

}  // namespace services_com
