# artheia gen-proto: `package system.supervisor` collides with libc `system()` — RESOLVED

## Symptom (was)

After running:

```
artheia gen-proto platform/supervisor/system/package.art \
    --out platform/supervisor/generated/proto
```

…the supervisor build failed:

```
ControlRequest.pb.cc:640:32: error: 'system' in namespace '::' does not name a type
template<> ::system::supervisor::ControlRequest* Arena::CreateMaybeMessage<...>
                ^~~~~~
note: 'int system(const char*)' declared here  (/usr/include/stdlib.h:791:12)
```

protoc's generated `::system::supervisor::*` C++ qualifier was being
parsed by the compiler as a reference to the libc `int system(const
char*)` function from `<stdlib.h>`, which any of the .pb.cc TUs
include transitively.

## Root cause

The .art's `package system.supervisor` is the canonical FQN in the
Theia DSL (mirrors the directory layout under `platform/system/`),
but protoc maps it 1:1 to C++ `namespace system { namespace
supervisor`, colliding with libc's `int system(const char*)`.

## Fix (landed)

`artheia/artheia/generators/proto.py` now exposes a
`_proto_package_name()` helper that rewrites colliding leading
segments at gen-proto time:

```python
_PROTO_PACKAGE_LEAD_RENAMES = {
    "system": "services",   # libc system()
    # Add more here as new collisions appear.
}
```

The .art DSL stays canonical — `package system.supervisor` reads
naturally in the .art ecosystem — but the emitted .proto's `package`
line is `services.supervisor`, which protoc maps to
`::services::supervisor::*`. C++ code references types under
`services::supervisor::*` (which it already did, since the generated
.protos had been hand-patched to that name).

`proto_package.py` (the other generator) imports + uses the same
helper so behavior is consistent across both code paths.

## Verification

- `artheia gen-proto platform/supervisor/system/package.art ...` emits
  `.protos` with `package services.supervisor;` and no longer requires
  the manual `sed` patch.
- `cmake --build platform/supervisor/build` → green.
- `cmake --build services/com/build` → green.
- `tools/supdbg/gen_protos.sh` regenerates 37 modules; supdbg imports
  cleanly.

## Mapping table (future additions)

The renames table is intentionally narrow — only redirect leading
segments that ARE known C/POSIX identifiers, since arbitrary
rewriting would surprise users. Candidates to add when they ever
appear:

- `time` (libc time())
- `exit` (libc exit())
- `kill` (libc kill())

For now `system` is the only one.
