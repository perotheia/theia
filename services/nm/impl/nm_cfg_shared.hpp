// nm_cfg_shared — the process-global config-transaction state shared between
// NmCfgGate (owner: validates + mutates + post_events) and NmCfgTxn (the FSM:
// applies via per on_enter). Same in-process-singleton pattern as
// NmPoller's nm_statem_ref(): the gate has the event payload (handle_call), the
// statem only gets (new,old,data) in on_enter — so the gate stashes the
// computed NmConfig here and the FSM reads it. One writer (gate, on the gate
// thread) → one reader (txn, on the FSM thread) per transition; the post_event
// in between is the happens-before barrier, so a plain struct + the FSM mailbox
// ordering is enough (no extra lock).
#pragma once

#include "system/services/nm/nm.pb.h"   // system_services_nm_NmConfig

#include "NodeRef.hh"       // theia::runtime::LocalRef
#include "GenStateM.hh"     // theia::runtime::post_event

namespace system_services_nm {

// Forward-decl the FSM so the gate can post_event into it without including the
// full NmCfgTxn lib (avoids a header cycle gate↔txn).
class NmCfgTxn;

struct NmCfgShared {
    // The last-COMMITTED config (rollback target) + the PENDING one the gate
    // computed for the in-flight transaction. Both full NmConfig by value.
    system_services_nm_NmConfig committed = system_services_nm_NmConfig_init_zero;
    system_services_nm_NmConfig pending   = system_services_nm_NmConfig_init_zero;
    bool committed_known = false;   // false until the gate first reads per
    bool txn_pending     = false;   // a transaction is awaiting confirm
};

// The process-global instance (defined in NmCfgGate_handlers.cc).
NmCfgShared& nm_cfg_shared();

// The LocalRef the FSM publishes itself into (NmCfgTxn::on_enter on first entry),
// so the gate can post_event Txn* into it. Defined in NmCfgTxn_handlers.cc.
::theia::runtime::LocalRef<NmCfgTxn>& nm_cfg_txn_ref();

}  // namespace system_services_nm
