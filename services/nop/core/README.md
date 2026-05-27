# core — conceptual only

AUTOSAR `ara::core` is the foundational library — types like
`ara::core::Result`, `Future`, `StringView` — not a daemon.

The conceptual carve-out lives here so future `ara/include/*.h`
headers can land under `services/system/core/include/`.

**Status:** placeholder. `package.art` keeps the TIPC slot only.
There is no Core daemon.
