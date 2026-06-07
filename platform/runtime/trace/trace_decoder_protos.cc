// Static-init shim: registers every libprotobuf-compiled message
// type with the process-global Decoder. Adding a new type to the
// libtrace_decoder.so means (a) adding the corresponding sm_pb_cpp-
// style cc_library dep on this target's BUILD and (b) adding one
// REGISTER line below.
//
// This file is intentionally separate from trace_decoder.cc so the
// core library stays proto-agnostic — useful when rtdb or the
// supervisor-gui want their own message subset.

#include "trace_decoder.hh"

#include "sm.pb.h"  // libprotobuf header via //platform/proto/.../sm:sm_pb_cpp

namespace {

struct Registrar {
    Registrar() {
        artheia::trace::register_global(
            "SmStateMsg",
            &system_services_sm::SmStateMsg::default_instance());
        artheia::trace::register_global(
            "SmRequest",
            &system_services_sm::SmRequest::default_instance());
    }
};

[[maybe_unused]] static const Registrar _r{};

}  // namespace
