*** Settings ***
Documentation     Crash-investigation trace persistence via the com gRPC bridge.
...
...               The operator arms tracing for the sm module through com's
...               gRPC ConfigureTrace, reads the armed config back from the
...               supervisor (the persistence authority), then crashes the sm
...               child. The supervisor restarts sm and RE-APPLIES the stored
...               trace config to the fresh child automatically — so the trace
...               you set to investigate a fault survives the fault.
...
...               Path: rf → com (gRPC) → supervisor (TIPC, op_kind 9/10) → node.
...               com is the bridge; the supervisor owns trace_configs_ and the
...               heartbeat-after-gap re-push (#361).
Library           trace_crash_investigation_lib.py
Suite Setup       Stage And Start Central
Suite Teardown    Stop Crash Investigation Stack

*** Test Cases ***
Trace Survives An Sm Crash And Is Re-Applied On Restart
    [Documentation]    Set trace for sm via com, confirm the supervisor
    ...                remembers it, crash sm, and assert the supervisor
    ...                re-pushes the same config to the restarted child.
    # com bridge connects to the supervisor's TIPC at startup.
    Start Com Bridge

    # 1. Arm: rf → com.ConfigureTrace(sm, SmStateMsg, TK_STATEM).
    Activate Sm Statem Trace Via Com
    Wait For Supervisor Push To Sm

    # 2. Read back from the supervisor (the crash-investigation view of
    #    "what tracing is armed and will be re-applied").
    ${configs}=    Read Back Trace Config From Supervisor
    Trace Config Should Contain Sm Statem    ${configs}

    # 3. Record how many times the supervisor has pushed trace so far, then
    #    crash sm (SIGKILL the child process — a real crash, not a graceful
    #    RestartChild).
    ${baseline}=    Note Trace Push Count
    Crash The Sm Child

    # 4. The supervisor restarts sm and re-applies the stored trace config
    #    on the first heartbeat-after-gap — a NEW push beyond the baseline.
    Supervisor Reapplied Trace After Restart    ${baseline}

    # 5. And the read-back still shows the armed entry after the restart:
    #    persistence is durable, not consumed by the push.
    ${after}=    Read Back Trace Config From Supervisor
    Trace Config Should Contain Sm Statem    ${after}
