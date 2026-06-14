// Static-init shim: registers every libprotobuf-compiled message
// type with the process-global Decoder. Adding a new type to the
// libtrace_decoder.so means (a) adding the corresponding sm_pb_cpp-
// style cc_library dep on this target's BUILD and (b) adding one
// REGISTER line below.
//
// This file is intentionally separate from trace_decoder.cc so the
// core library stays proto-agnostic — useful when rtdb or the
// supervisor-gui want their own message subset.
//
// REGISTRATION KEY = the WIRE name the trace record carries: the
// nanopb-flattened type name `msg_type_name<T>()` emits (it's `#T`,
// e.g. `system_services_sm_SmStateMsg`, `system_apps_Inc`), NOT the
// short C++ class name. The decoder looks the record's msg_type up
// verbatim, so the keys MUST match the wire names or every lookup
// misses. (This was previously registered under short names like
// "SmStateMsg" — a latent bug: no trace ever decoded.)

#include "trace_decoder.hh"

#include "sm.pb.h"    // //platform/proto/system/services/sm:sm_pb_cpp
#include "apps.pb.h"  // //platform/proto/system/apps:apps_pb_cpp

namespace {

struct Registrar {
    Registrar() {
        using artheia::trace::register_global;

        // sm — the state machine's wire messages.
        register_global("system_services_sm_SmStateMsg",
                        &system_services_sm::SmStateMsg::default_instance());
        register_global("system_services_sm_SmRequest",
                        &system_services_sm::SmRequest::default_instance());

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
