// RemoteCodec specializations for the platform/runtime test suite's
// own nanopb types. Kept inside the test directory so the cc_test
// target stays self-contained — no dependency on any consuming workspace or
// downstream app's message catalog.

#pragma once

#include "RemoteCodec.hh"

#include "platform_runtime_test/messages.pb.h"

THEIA_DECLARE_REMOTE_CODEC(platform_runtime_test_Inc);
THEIA_DECLARE_REMOTE_CODEC(platform_runtime_test_Get);
THEIA_DECLARE_REMOTE_CODEC(platform_runtime_test_GetReply);
