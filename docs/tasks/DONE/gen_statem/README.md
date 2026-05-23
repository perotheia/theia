# gen_statem — FSM support in platform/runtime

`platform/runtime/` needs FSM implementation to be used in applications
and services.

**Status**: DONE 2026-05-23. All five phases shipped — runtime,
grammar, codegen, and the .art declaration on `services/system/sm/`.
The `sm` binary itself is not built yet; see `sm-daemon-example.md`
for the shape the eventual consumer should take.

## Proposal (locked)

- Reuse Erlang `gen_statem` from `up/otp` as the user-facing API:
  - `otp/lib/stdlib/src/gen_statem.erl` — reference behaviour (5413 LOC)
  - `otp/lib/stdlib/test/gen_statem_SUITE_data/oc_statem.erl`
  - `otp/lib/stdlib/test/gen_statem_SUITE_data/format_status_statem.erl`
  - `otp/lib/stdlib/test/gen_statem_SUITE.erl`
  - `otp/lib/ssl/src/ssl_gen_statem.erl` (real-world consumer)
- Reuse `up/hsmcpp/` C++ FSM as FSM definition-syntax inspiration
  (states / events / transitions declared once, generator emits a
  base class). NOT its SCXML format — we use artheia instead.
- Layer on the existing `platform/runtime/GenServer` actor pattern
  (per-node thread + mailbox + handle_call/cast/info + tracer).

## Scope (Option A + C, confirmed 2026-05-23)

1. **A** — Minimal C++ `GenStateM<Derived, StateT, DataT>` in
   `platform/runtime/`.
2. **C** — `.art` grammar gains a `statem { ... }` block; codegen
   (`gen-cpp-stubs`) emits `<Name>StateMBase.hpp` from it.
3. First consumer (split commits):
   - example tests in `platform/runtime/test/` (traffic-light, retry,
     guards) → land with MVP.
   - `sm` FC migrates to GenStateM → follow-up commit.

## Phases

1. ✅ Design — locked in `design.md`.
2. ✅ C++ `GenStateM.hh` + 4 unit tests (traffic-light, retry-escalate,
   postpone, door-lock-guards). Lands in theia `f711517`.
3. ✅ artheia grammar `statem { ... }` + AST plumbing + LSP keywords.
   Lands in artheia `852d84d` (and theia `a0bb968` design-doc update).
4. ✅ `gen-cpp-stubs` lowering: emit `<Name>StateMBase.hh`. Lands in
   artheia `aba18fa`.
5. ✅ `sm` migration — `statem` block declared on `SmDaemon` in
   `services/system/sm/package.art`; `sm-daemon-example.md` shows
   the consumer skeleton for when the binary actually gets built.
6. (deferred) supdbg `statem` subcommand for live state introspection.

See `design.md` for the full API surface, grammar, lowering rules,
SM walkthrough, and test plan. See `sm-daemon-example.md` for the
example C++ consumer that the eventual `services/system/sm/` binary
should follow.
