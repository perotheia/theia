*** Settings ***
Documentation     RDS zero-copy data plane — consistent reporting wrapper.
...
...               ara::rds is the bulk DATA plane: iceoryx shared memory carries
...               large payloads (camera frames) producer→consumer with ZERO
...               copy, while the Theia TIPC control plane carries the "frame
...               ready" notification. RouDi (iox-roudi, a supervised native
...               child) owns the pools. A node opts in via the .art
...               `requires_rds` + `rds_stream` (gen-app links librds + emits the
...               StreamWriter/Reader handles).
...
...               This suite runs the cc roundtrip (//services/rds/test:
...               rds_roundtrip) — a writer Loans/fills/Publishes a frame, a
...               reader Takes the SAME shared-memory address (zero copy) — and
...               reports its verdict. Needs iox-roudi running (the supervised
...               broker); SKIPs cleanly otherwise.
Library           ${CURDIR}/rds_lib.py

Force Tags        services-rds    live    zero-copy


*** Test Cases ***
Rds Frame Roundtrips Zero-Copy Over RouDi
    [Documentation]    A FrameDescriptor + payload Loaned/Published by the writer
    ...                is Taken by the reader at the identical shared-memory
    ...                pointer (no copy), payload intact. A regression that
    ...                breaks the iceoryx transport or the zero-copy contract
    ...                fails this case.
    Require RouDi Running
    Run Rds Roundtrip
