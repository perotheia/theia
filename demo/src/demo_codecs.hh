// RemoteCodec<T> specializations for every C++ message type the demo
// carries over TIPC. Each specialization declares the message's
// service_id (djb2 hash of the type name) and its nanopb _fields
// descriptor. The mux registers handlers under the same service_id;
// the sender stamps the same id; the receiver looks up by id.
//
// Today hand-written. Once artheia gains a codegen pass that walks the
// .art `message` decls and emits RemoteCodec specializations, this
// file becomes generated output.

#pragma once

#include "NodeRef.hh"

#include "demo/system/system.pb.h"

DEMO_DECLARE_REMOTE_CODEC(demo_system_Inc);
DEMO_DECLARE_REMOTE_CODEC(demo_system_Get);
DEMO_DECLARE_REMOTE_CODEC(demo_system_GetReply);
