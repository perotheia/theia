// SM message types — thin C++ alias layer over the nanopb-generated
// C structs in platform/proto/system/services/sm/sm.pb.h.
//
// The nanopb generator emits long C-style names prefixed with the
// libc-safe proto package (services_services_sm_SystemBoot, etc.).
// This header maps each to a short C++ alias inside the
// system_services_sm namespace so the daemon code stays readable.
//
// Workflow:
//
//   .art (services/system/sm/package.art)
//      ↓  artheia gen-proto-package
//   .proto (platform/proto/system/services/sm/sm.proto)
//      ↓  nanopb_generator
//   .pb.{h,c} (platform/proto/system/services/sm/sm.pb.{h,c})
//      ↓  THIS HEADER (hand-written aliases)
//   sm_daemon.hh / SmDaemonStateMBase.hh — use the short names

#pragma once

#include "system/services/sm/sm.pb.h"

namespace system_services_sm {

// Aliases for the nanopb C types — same memory layout, shorter
// names. POD throughout, so passing by value / reference is free.
//
// SmState is an enum (nanopb emits a C `typedef enum`); the others
// are `typedef struct`s.

using SmStateMsg      = services_services_sm_SmStateMsg;
using SmRequest       = services_services_sm_SmRequest;
using SmEmpty         = services_services_sm_SmEmpty;
using SystemBoot      = services_services_sm_SystemBoot;
using StartupComplete = services_services_sm_StartupComplete;
using ShutdownRequest = services_services_sm_ShutdownRequest;
using UpdateRequest   = services_services_sm_UpdateRequest;
using UpdateComplete  = services_services_sm_UpdateComplete;
using RetryStartup    = services_services_sm_RetryStartup;
using PowerOff        = services_services_sm_PowerOff;

// SmState: alias the enum type itself + each value (the C-side
// enum values are prefixed; the short names below match what was
// in the original hand-rolled sm_messages.hh).
using SmState = services_services_sm_SmState;
constexpr SmState SmState_OFF       = services_services_sm_SmState_OFF;
constexpr SmState SmState_STARTING  = services_services_sm_SmState_STARTING;
constexpr SmState SmState_RUNNING   = services_services_sm_SmState_RUNNING;
constexpr SmState SmState_DEGRADED  = services_services_sm_SmState_DEGRADED;
constexpr SmState SmState_UPDATE    = services_services_sm_SmState_UPDATE;
constexpr SmState SmState_SHUTDOWN  = services_services_sm_SmState_SHUTDOWN;

}  // namespace system_services_sm
