// Static-init shim: registers the APP (system_apps_*) wire types with
// THIS plugin .so's process-global Decoder. This is the consuming
// workspace's half of the pluggable trace-decoder split — the framework
// (system_services_sm_*) types live in @pero_theia's
// libtrace_decoder_system.so; the app types the demo emits live here, in
// libtrace_decoder_apps.so.
//
// PLUGGABLE DECODER MODEL: each libtrace_decoder_*.so carries its OWN
// copy of the global Decoder + its OWN `trace_decode` C ABI. A consumer
// dlopen()s every plugin in its plugin dir and tries each plugin's
// `trace_decode` in turn until one returns >0. So this TU registers ONLY
// the app's types; framework types come from the system plugin.
//
// REGISTRATION KEY = the WIRE name the trace record carries: the
// nanopb-flattened type name `msg_type_name<T>()` emits (it's `#T`,
// e.g. `system_apps_Inc`), NOT the short C++ class name. These keys are
// copied verbatim from the original monolithic trace_decoder_protos.cc
// (its app half) so existing records keep decoding.

#include "trace_decoder.hh"               // @pero_theia core C ABI + register_global

#include "system/apps/apps.pb.h"          // //proto/system/apps:apps_pb_cpp

namespace {

struct Registrar {
    Registrar() {
        using artheia::trace::register_global;

        // demo — the gen_server API demo's message set (Inc / Get / GetReply).
        // These are the records the GUI/rtdb show against the running demo.
        register_global("system_apps_Inc",
                        &system_apps::Inc::default_instance());
        register_global("system_apps_Get",
                        &system_apps::Get::default_instance());
        register_global("system_apps_GetReply",
                        &system_apps::GetReply::default_instance());

        // demo CONFIG messages — the structured config stored in etcd
        // (/theia/config/<node>). Registering them lets the supervisor-gui
        // Table Viewer decode a selected config value proto→JSON instead of
        // showing a hex dump (the same TraceDecoderLib path traces use).
        register_global("system_apps_CounterConfig",
                        &system_apps::CounterConfig::default_instance());
        register_global("system_apps_ObserverConfig",
                        &system_apps::ObserverConfig::default_instance());
        register_global("system_apps_IncrementerConfig",
                        &system_apps::IncrementerConfig::default_instance());
        register_global("system_apps_P4Config",
                        &system_apps::P4Config::default_instance());
    }
};

[[maybe_unused]] static const Registrar _r{};

}  // namespace

// ---------------------------------------------------------------------------
// Release-version C ABI — the APP's OWN version, distinct from the
// framework system plugin's. The pluggable-decoder loader reads every
// plugin's value and warns (does NOT hard-fail) if an app plugin's version
// disagrees with the framework system plugin's — a wire-format drift
// early-warning. Kept SIMPLE: a compile-time constant the consuming
// workspace bumps when it rev's its app protos; CI may stamp it from the
// workspace's own `git describe`.
// ---------------------------------------------------------------------------
#ifndef THEIA_TRACE_DECODER_APPS_VER
#define THEIA_TRACE_DECODER_APPS_VER "0.1.0"
#endif

extern "C" const char* trace_decoder_release_ver(void) {
    return THEIA_TRACE_DECODER_APPS_VER;
}
