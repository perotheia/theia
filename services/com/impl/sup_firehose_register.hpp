// register_firehose_casts — install the supervisor's #429 topo-pair firehose
// stream dispatch on a ComDaemon TIPC binding, feeding SupFirehose.
//
// Confined declaration: main.cc calls this with the opaque NodeBinding* it got
// from config_mux.bind_node(com_daemon, …). The IMPLEMENTATION
// (sup_firehose_register.cc) owns the supervisor NANOPB structs +
// supervisor_codecs.hh; main.cc never sees a supervisor .pb.h, so the
// same-basename nanopb / libprotobuf headers never collide on main.cc's
// include path. Same isolation rationale as impl/sup_link.

#pragma once

namespace demo { namespace runtime { struct NodeBinding; } }

namespace services_com {

// Register Cast dispatch entries for SnapshotBegin / NodeEdge / NodeState /
// SnapshotEnd on `b` (ComDaemon's binding). Each decodes the nanopb struct on
// the mux epoll thread and feeds SupFirehose::instance(). Idempotent per
// binding (overwrites any prior entry for the same service_id).
void register_firehose_casts(demo::runtime::NodeBinding* b);

}  // namespace services_com
