// RemoteCodec specializations for the supervisor's TIPC control surface.
//
// The com<->supervisor link is the standard Theia transport: TipcMux +
// RemoteRef + RemoteCodec over the nanopb GwMessageHeader wire (see
// docs/com-supervisor-transport.md). RemoteCodec<T> binds each nanopb
// message type to its service_id (= djb2_low16 of the C type name) and its
// nanopb field descriptor, so register_call / RemoteRef dispatch by
// service_id exactly like every FC message.
//
// The control path is request/reply over the ControlRequest envelope:
//   register_call<ControlRequest, ControlReply>
// so those two are the codecs the transport actually needs. The nested
// request bodies (ConfigureTraceRequest, …) ride INSIDE ControlRequest and
// the replies (TraceConfigList) ride INLINE in ControlReply.trace_config_list
// — they are decoded by nanopb as sub-messages, not dispatched by their own
// service_id, so they need no codec here.
//
// BOTH ends include this header: the supervisor's SupervisorControlNode
// (register_call) and com's RemoteRef must hash the SAME C type name to the
// SAME service_id. The nanopb binding is //platform/supervisor:supervisor_nanopb.

#pragma once

#include "RemoteCodec.hh"          // DEMO_DECLARE_REMOTE_CODEC (platform/runtime)
#include "ControlRequest.pb.h"     // nanopb (supervisor_nanopb include root)
#include "ControlReply.pb.h"

DEMO_DECLARE_REMOTE_CODEC(services_supervisor_ControlRequest)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_ControlReply)
