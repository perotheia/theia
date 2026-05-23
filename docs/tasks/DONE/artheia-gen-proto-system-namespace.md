# artheia gen-proto: `package system.supervisor` collides with libc `system()`

## Symptom

After running:

```
artheia gen-proto platform/supervisor/system/package.art \
    --out platform/supervisor/generated/proto
```

…the supervisor build fails:

```
ControlRequest.pb.cc:640:32: error: 'system' in namespace '::' does not name a type
template<> ::system::supervisor::ControlRequest* Arena::CreateMaybeMessage<...>
                ^~~~~~
note: 'int system(const char*)' declared here  (/usr/include/stdlib.h:791:12)
```

protoc's generated `::system::supervisor::*` C++ qualifier is being
parsed by the compiler as a reference to the libc `int system(const char*)`
function from `<stdlib.h>`, which any of the .pb.cc TUs include
transitively.

## Root cause

`platform/supervisor/system/package.art` declares
`package system.supervisor`. The proto template in
`artheia/artheia/generators/proto.py` emits this verbatim as
`package system.supervisor;` in every generated .proto. protoc then
maps that to C++ `namespace system { namespace supervisor { ... }}`.

`system` is a name from libc-with-no-namespace, and any compilation
unit that includes both `<stdlib.h>` (or anything pulling it in)
*and* a Theia .pb.h with `::system::supervisor::*` fully-qualified
references hits this collision.

## Existing workaround

The in-tree `platform/supervisor/generated/proto/*.proto` files
have a hand-patched `package services.supervisor;` line. The C++
source under `platform/supervisor/src/` and `services/com/src/`
already references types as `::services::supervisor::*`. Everything
compiles as long as nobody runs `artheia gen-proto` again — which
blows the patch away.

Re-applying the patch:

```
sed -i 's/^package system\.supervisor;/package services.supervisor;/' \
    platform/supervisor/generated/proto/*.proto
```

## Real fixes (pick one)

1. **Rename the .art package.** Change `package system.supervisor`
   to `package services.supervisor` in
   `platform/supervisor/system/package.art`. Cascades: every .art
   that imports the supervisor's types would need the import path
   updated. Lowest tooling risk.

2. **Make artheia gen-proto avoid the collision.** Either:
   - Reject any package path whose first component is a libc-known
     identifier (`system`, `time`, `exit`, etc.) at gen-proto time.
   - Emit `option cpp_namespace = "art_<pkg>"` so the C++ qualifier
     becomes `::art_system::supervisor::*` regardless of the proto
     package. Loses readability slightly but isolates C++ from any
     libc identifier.

3. **Force a different cpp namespace via .proto's `option`.** Add
   `option (cc_generic_services) = false;` plus an explicit
   `option java_package` / equivalent for the C++ side. Less clean
   than #2; couples to protoc's option set.

Recommendation: do #1 first (single sed across two files +
imports), and file #2 as a smaller artheia improvement later.

## Why this isn't fixed in this session

User asked for SystemInfo RPC + other GUI/backend work. Mid-D1 I
hit this when regenerating the .protos for a new message. Rolled
back the .art changes + re-patched the .protos manually with the
sed above; build is green again. The architectural .art rename
is its own task (touches potentially several files outside the
supervisor) and shouldn't be hidden inside a GUI-related commit.
