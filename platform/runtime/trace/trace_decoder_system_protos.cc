// Static-init shim: registers every FRAMEWORK libprotobuf-compiled
// message type with this .so's process-global Decoder. This is the
// SYSTEM (framework) half of what was once the monolithic
// trace_decoder_protos.cc — the demo/app types (`system_apps_*`)
// were split out into the consuming workspace's own plugin
// (demo/trace/trace_decoder_apps_protos.cc → libtrace_decoder_apps.so).
//
// PLUGGABLE DECODER MODEL: each libtrace_decoder_*.so carries its OWN
// copy of the global Decoder + its OWN `trace_decode` C ABI. A consumer
// dlopen()s every plugin in its plugin dir and tries each plugin's
// `trace_decode` in turn until one returns >0. So this TU registers
// only the framework's types; app types live in the app's own plugin.
//
// Adding a new FRAMEWORK type means (a) adding the corresponding
// sm_pb_cpp-style cc_library dep on this target's BUILD and (b) adding
// one register_global line below.
//
// REGISTRATION KEY = the WIRE name the trace record carries: the
// nanopb-flattened type name `msg_type_name<T>()` emits (it's `#T`,
// e.g. `system_services_sm_SmStateMsg`), NOT the short C++ class name.
// The decoder looks the record's msg_type up verbatim, so the keys
// MUST match the wire names or every lookup misses.

#include "trace_decoder.hh"

#include "sm.pb.h"    // //platform/proto/system/services/sm:sm_pb_cpp
// platform.msgs — the ROS-analogue payload types broadcast over PG (e.g.
// tsync's RTK-native GnssSolution + Imu GPS feed). Registered so `rtdb tracecat`
// + the GUI decode them instead of leaving the payload raw.
#include "platform/msgs/sensor/sensor.pb.h"  // sensor_pb_cpp
#include "platform/msgs/nav/nav.pb.h"        // nav_pb_cpp
#include "nm.pb.h"    // //platform/proto/system/services/nm:nm_pb_cpp

namespace {

struct Registrar {
    Registrar() {
        using artheia::trace::register_global;

        // sm — the state machine's wire messages.
        register_global("system_services_sm_SmStateMsg",
                        &system_services_sm::SmStateMsg::default_instance());
        register_global("system_services_sm_SmRequest",
                        &system_services_sm::SmRequest::default_instance());

        // platform.msgs.sensor — GPS fix + IMU + camera.
        register_global("platform_msgs_sensor_NavSatFix",
                        &platform_msgs_sensor::NavSatFix::default_instance());
        register_global("platform_msgs_sensor_Imu",
                        &platform_msgs_sensor::Imu::default_instance());
        // platform.msgs.nav — the RTK-native GnssSolution (tsync's primary GPS
        // broadcast: position + velocity + RTK quality) + the legacy Odometry.
        register_global("platform_msgs_nav_GnssSolution",
                        &platform_msgs_nav::GnssSolution::default_instance());
        register_global("platform_msgs_nav_Odometry",
                        &platform_msgs_nav::Odometry::default_instance());

        // nm — the config-transaction FSM data (Assert Statem Data in the nm FC
        // test) + the network-readiness broadcast (temporal-logic assertions).
        register_global("system_services_nm_NmCfgTxnData",
                        &system_services_nm::NmCfgTxnData::default_instance());
        register_global("system_services_nm_NmStatusMsg",
                        &system_services_nm::NmStatusMsg::default_instance());
    }
};

[[maybe_unused]] static const Registrar _r{};

}  // namespace

// ---------------------------------------------------------------------------
// Release-version C ABI — lets the loader read+compare each plugin's
// version. SINGLE SOURCE OF TRUTH for the FRAMEWORK (system) plugin
// version. Kept SIMPLE: a compile-time constant. CI should stamp this
// (e.g. from `git describe`) the same way platform/supervisor/impl's
// version_h genrule stamps THEIA_GIT_SHA; until then a plain semver
// string is enough for the loader to read + warn on mismatch.
// ---------------------------------------------------------------------------
#ifndef THEIA_TRACE_DECODER_SYSTEM_VER
#define THEIA_TRACE_DECODER_SYSTEM_VER "0.1.0"
#endif

extern "C" const char* trace_decoder_release_ver(void) {
    return THEIA_TRACE_DECODER_SYSTEM_VER;
}
