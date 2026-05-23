# phm — not implemented

AUTOSAR Platform Health Management. In Theia the supervisor samples
`/proc/<pid>/stat` directly and reports `ChildState` upward via gRPC,
so phm has nothing to do.

**Status:** placeholder. `package.art` keeps the TIPC slot only.
