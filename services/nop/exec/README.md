# exec — not implemented (functionality lives in `platform/supervisor/`)

AUTOSAR Execution Management — start/stop/restart of processes,
dependency ordering, sandboxing. Implemented in Theia by the
**supervisor**, which is the OTP-style supervisor tree in
`platform/supervisor/`. There is no separate `exec` daemon.

**Status:** placeholder. `package.art` keeps the TIPC slot only.
