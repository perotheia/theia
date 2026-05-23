// SM message types — POD structs (no protobuf wire encoding yet).
//
// These mirror the message declarations in
// services/system/sm/package.art. Once SM gets a proto schema +
// codegen wired into the Bazel build, this file is replaced by the
// auto-generated .pb.h. Until then, hand-rolled POD lets us drive
// the FSM in-process (cast via LocalRef) and integration-test
// without paying the protobuf+TIPC stack tax.
//
// The shape MUST stay in lockstep with package.art. The integration
// test asserts via the .art-declared event names; drift between this
// file and the .art is the kind of thing artheia audit-manifest will
// (eventually) catch.

#pragma once

#include <cstdint>

namespace system_services_sm {

// SmState — kept here for the in-process world; identical to the
// .art enum SmState. The auto-generated SmDaemonStateMBase.hh
// declares SmDaemonState (one-per-node enum naming convention) and
// we keep these two in sync via a static_assert in the daemon.
enum class SmState : uint8_t {
    OFF       = 0,
    STARTING  = 1,
    RUNNING   = 2,
    DEGRADED  = 3,
    UPDATE    = 4,
    SHUTDOWN  = 5,
};

// Data carried through the FSM. Doubles as the broadcast payload on
// SmStateStream (sender port). `state` mirrors the FSM's current
// state — on_enter mutates this before cast()ing.
struct SmStateMsg {
    SmState  state{SmState::OFF};
    uint64_t ts_ns{0};
};

// External inbound (StateMgmtCtl) — mode-change request.
struct SmRequest { SmState target{SmState::OFF}; };
struct SmEmpty   { };

// FSM-internal events. Posted via post_event() from sibling FCs;
// SmDaemon's handle_call(SmRequest) translates inbound requests
// into these as well.
struct SystemBoot       { };
struct StartupComplete  { };
struct ShutdownRequest  { };
struct UpdateRequest    { };
struct UpdateComplete   { };
struct RetryStartup     { };
struct PowerOff         { };

}  // namespace system_services_sm
