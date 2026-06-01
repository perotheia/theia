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

// #429 — the topo-pair firehose stream messages. The supervisor CASTs these
// (GW_MSG_GEN_CAST) to com's ComDaemon; com register_casts the same C type
// names so the djb2 service_ids match. They have no reply, so they need only
// the cast codec — same as LogLevelPush/TraceControlPush.
#include "NodeEdge.pb.h"
#include "NodeState.pb.h"
#include "SnapshotBegin.pb.h"
#include "SnapshotEnd.pb.h"

DEMO_DECLARE_REMOTE_CODEC(services_supervisor_ControlRequest)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_ControlReply)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_NodeEdge)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_NodeState)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_SnapshotBegin)
DEMO_DECLARE_REMOTE_CODEC(services_supervisor_SnapshotEnd)
