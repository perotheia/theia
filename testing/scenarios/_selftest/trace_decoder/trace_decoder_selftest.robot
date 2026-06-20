*** Settings ***
Documentation    Hermetic selftest of the rf-theia trace_decoder
...              adapter (#356).
...
...              Demonstrates the full producer→consumer path for
...              one trace record without a live cluster:
...
...                runtime/Tracer.hh (C++)  ──nanopb wire bytes──►
...                libtrace_decoder_system.so (ctypes plugin)     ──►
...                rf_theia.adapters.trace_decoder.TraceDecoder   ──►
...                Python dict
...
...              The 12-byte payload below is the canonical SmStateMsg
...              encoding of {state=RUNNING, ts_ns=1700000000123456789}
...              — it's what the platform/runtime/trace round-trip
...              cc_test produces (#359). If proto-wire-v3 stayed
...              stable between the two stacks for that one message,
...              the design holds; scaling out is just registering
...              more prototypes in trace_decoder_system_protos.cc
...              (framework) / an app workspace's
...              trace_decoder_apps_protos.cc.
...
...              Prereq: `bazel build
...                //platform/runtime/trace:libtrace_decoder_system.so`.
Library          ${CURDIR}/trace_decoder_lib.py


Suite Setup       Open Trace Decoder


*** Variables ***
# nanopb encoding of SmStateMsg{state=RUNNING(=2), ts_ns=1700000000123456789}.
# Captured from the platform/runtime/trace cc_test fixture; the same
# bytes the C++ Decoder consumes in trace_decoder_roundtrip_test.cc.
${RUNNING_PAYLOAD_HEX}    080210959a97ece39fe7cb17


*** Test Cases ***
SO Has Expected Prototypes Registered
    [Documentation]    The trace_decoder_system_protos.cc static-init shim
    ...                must register at least one message type. If the
    ...                count is 0, the static `Registrar` ctor didn't
    ...                run (linker drop) — alwayslink=True is missing.
    [Tags]    trace-decoder    hermetic    selftest

    ${n}=    Trace Decoder Registered Count
    Should Be True    ${n} >= 1    .so was loaded with zero prototypes


Decode SmStateMsg Round Trip
    [Documentation]    The single most important assertion: a payload
    ...                produced by nanopb decodes via libprotobuf
    ...                reflection into a structured dict with the
    ...                fields we put in. If this fails, every higher-
    ...                level trace consumer (supdbg, supervisor-gui,
    ...                rf-theia assertions) is broken at the wire
    ...                layer.
    [Tags]    trace-decoder    hermetic    selftest    priority-high

    ${decoded}=    Decode Trace Payload Hex    SmStateMsg    ${RUNNING_PAYLOAD_HEX}

    # proto3 JSON: enum surfaces as its string name; uint64 as string
    # (to dodge JS precision loss in JSON parsers — same in supdbg).
    Should Be Equal As Strings    ${decoded}[state]    RUNNING
    Should Be Equal As Strings    ${decoded}[tsNs]     1700000000123456789


Empty Payload Decodes To Defaults
    [Documentation]    proto3 with always_print_primitive_fields means
    ...                an empty payload still produces field entries —
    ...                handy for "no event yet" rendering.
    [Tags]    trace-decoder    hermetic    selftest

    ${decoded}=    Decode Trace Payload Hex    SmStateMsg    ${EMPTY}

    Should Be Equal As Strings    ${decoded}[state]    OFF
    Should Be Equal As Strings    ${decoded}[tsNs]     0


Unknown Message Type Returns Clear Error
    [Documentation]    A type the .so wasn't built with should fail
    ...                fast with a useful error — not silently produce
    ...                garbage JSON.
    [Tags]    trace-decoder    hermetic    selftest

    ${err}=    Decode Trace Payload Hex Expecting Error
    ...        NoSuchType    ${RUNNING_PAYLOAD_HEX}
    Should Contain    ${err}    no prototype for message type
