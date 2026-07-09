// CI SEED — SensorCtrl handler bodies (USER-side, write-once slot of the
// harness's sensor package). A timer-driven Sample publisher: every rate_ms it
// broadcasts Sample{seq, value} on the SampleStream pg group. Exercises: params
// read at init, send_after timers, sender-port pg broadcast, a call surface.

#include "lib/SensorCtrl.hh"

#include "ParamsConfig.hh"   // get_config().node(kNodeName).u32(...)
#include "TimerService.hh"   // send_after / process_timers

#include <cstring>

namespace ara::sensor {

void SensorCtrl::init(SensorCtrlState& s) {
    // Static deploy knob (config/<process>.json → nodes.<this>.rate_ms).
    s.rate_ms = ::theia::runtime::get_config().node(kNodeName)
                    .u32("rate_ms", s.rate_ms);
    // Producer side of the SampleStream group: monitor membership so
    // broadcast_samples_out_sample() fans out to every joined consumer.
    pg_watch<Sample>();
    log().info("sensor up — publishing every " + std::to_string(s.rate_ms) + " ms");
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 static_cast<int>(s.rate_ms), *this, "tick");
}

void SensorCtrl::handle_info(const char* info, SensorCtrlState& s) {
    if (std::strcmp(info, "tick") != 0) return;
    Sample m{};
    m.seq   = ++s.seq;
    m.value = s.seq * 10u;
    broadcast_samples_out_sample(m);
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 static_cast<int>(s.rate_ms), *this, "tick");
}

SensorStatus SensorCtrl::handle_call(
        const SensorEmpty& /*req*/,
        SensorCtrlState& s) {
    SensorStatus st{};
    st.published = s.seq;
    st.ready     = true;
    return st;
}

}  // namespace ara::sensor
