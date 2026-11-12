# Theia on host `pascal` — the from-source recipe

Written by bond001, 2026-11-12, against theia `main` @ 8146d1c plus branch
`host-pascal` (theia + artheia). Host: Ubuntu 22.04, bazel 9.1.1 (apt), python
3.10, 32 cores, 62 GB. Everything below was run on this host; nothing is from
memory.

## Activation (every shell)

```sh
cd /home/axadmin/repo/theia            # or a consuming workspace: cd <ws> && source <theia>/env.sh
source env.sh                          # .venv + theia/tdb/rtdb on PATH, THEIA_ROOT
export USE_BAZEL_VERSION=9.1.1         # .bazelversion pins 9.1.0; only 9.1.1 is installed
```

`USE_BAZEL_VERSION` is needed in the framework AND in every scaffold
(`theia init` copies the 9.1.0 pin into the workspace's `.bazelversion`).

## One-time setup

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install -e artheia/ -e 'rf-theia/[mcp]'
pip install -e 'artheia/[probe]'       # grpcio-tools/protobuf: `theia call/cast` need it
pip install nanopb                     # 0.4.9.3: the genrules call `nanopb_generator`
                                       #   (22.04's apt nanopb only ships nanopb_generator.py)
pip install pytest pytest-snapshot     # only for the artheia/tools pytest suites
```

System prerequisites (apt, all installed with `--no-install-recommends`):

| package | version | why |
| --- | --- | --- |
| nanopb, libnanopb-dev | 0.4.5-2 | pb.h for the nanopb C structs |
| libgrpc++-dev, protobuf-compiler-grpc | 1.30.2-3build6 | `grpc_cpp_plugin` for com's gRPC bridge |
| libabsl-dev | 0~20210324.2-2ubuntu0.3 | grpc deps |
| libcpprest-dev | 2.10.18-1build2 | per (etcd-cpp-apiv3) |
| libnftables-dev | 1.0.2-1ubuntu3.1 | fw |
| libmnl-dev | 1.0.4-3build2 | nm |
| libacl1-dev | 2.3.1-1 | `-lacl` for ara::rds (iceoryx) |
| libbpf-dev | 1:0.5.0-1ubuntu22.04.1 | idsm's BPF program (`bpf/bpf_helpers.h`) |
| iceoryx | 1.0.1+dfsg-6 | `/usr/bin/iox-roudi` (RouDi) |
| libiceoryx-binding-c-dev, -posh-dev, -utils-dev | 1.0.1+dfsg-6 | ara::rds transport |

Already present: build-essential, cmake, pkg-config, libboost-*, libssl-dev,
libprotobuf-dev, protobuf-compiler 3.12.4, libnl-3/-route-3-dev, clang.
The list is `.github/workflows/selftest.yml`'s toolchain set plus
libacl1-dev, libbpf-dev and iceoryx, which CI never needs because it never builds
`//services/...:all`.

Kernel: `sudo modprobe tipc` (not persistent: redo after a reboot).

etcd (`per` needs `127.0.0.1:2379`; there is none on the host). Run the
upstream release binary AS THIS USER, never the apt `etcd-server` (that installs
and starts a root system service):

```sh
# one-time: etcd v3.5.17 linux-amd64 tarball, sha256-checked → /bigdata/work/bond001/tools/bin/
/bigdata/work/bond001/tools/bin/etcd --name dev \
  --data-dir /bigdata/work/bond001/tools/etcd-data \
  --listen-client-urls http://127.0.0.1:2379 --advertise-client-urls http://127.0.0.1:2379 \
  --listen-peer-urls http://127.0.0.1:2380 > /bigdata/work/bond001/tools/etcd.log 2>&1 &
```

Without etcd, `per` aborts ("failed to create a watch connection"), exceeds the
restart intensity and the supervisor tears the whole tree down.

## Build

```sh
artheia parse system/system.art
bazel build //services/...:all         # 504 actions cold, ~2 min on 32 cores
```

`~/.bazelrc` sets `--remote_cache=http://replace-with-your.host:8080` (a
placeholder). It only produces "Remote Cache" warnings; the build still
succeeds. Do NOT alternate `--remote_cache=` on and off: changing the flag
throws away the analysis cache and rebuilds everything.

## Run (a consuming workspace — `demo/` is gone)

`demo/` was retired in 5d0b3ed; its role went to `ci/run.sh`. The README's bare
`theia manifest` now needs a target. The framework root's own target
(`services`) is the fleet master/zonal deployment, not a dev stack. To run the
supervisor plus services locally, scaffold a workspace:

```sh
mkdir -p /bigdata/work/<you>/svc_ws && cd $_
source /home/axadmin/repo/theia/env.sh && export USE_BAZEL_VERSION=9.1.1
theia init --kind ws --name svc --with-services
artheia gen-fc system/svc/component.art --out apps --proto-out proto
artheia gen-manifest system/svc/component.art manifest/svc/manifest.py
theia manifest svc && theia install svc   # install sudo-setcaps the staged supervisor
theia start -d                            # FOREGROUND without -d
tdb ps                                    # 17 children: 16 FCs + the placeholder app
theia call SvcNode Ping --data '{}'
theia call RdsCtl GetStatus --data '{}'
theia cast PhmGate FcHealthReport --data '{"entity":"x","level":"HealthLevel_WARNING"}'
theia call PhmGate GetHealthStatus --data '{}'   # worst=1, n_entities=1
theia stop
```

Logs: `install/central/supervisor.log` (the supervisor and its children's
stdout) and `/tmp/theia/<fc>.log` (each FC's node logger: RouDi's output is in
`/tmp/theia/rds.log`).

Gotchas seen:
- `theia cast` enum fields take the PROTO label (`HealthLevel_WARNING`) or the
  number, not the `.art` label (`WARNING` → "unknown enum label").
- After the root supervisor escalates (e.g. per without etcd), the supervisor
  process stays alive with its pid file; `theia start` refuses
  ("already running") until `theia stop`.
- `theia start` seeds config defaults through `per`; without etcd it waits ~20 s
  and says so (non-fatal).

## RDS (ara::rds over iceoryx)

RouDi is the `rds` FC's `RoudiBroker` node: the supervisor forks it, so it runs
as the user who ran `theia start`. No systemd. Its `.art` default args are
`-c /etc/iceoryx/roudi_config.toml`, which doesn't exist on this host, and
iox-roudi exits ("could not be opened for parsing"). Point it at a
workspace-local mempool config instead (no `/etc` change):

```sh
mkdir -p deploy/iceoryx deploy/config/central
cp <a roudi_config.toml> deploy/iceoryx/          # see below
printf '{"nodes": {"roudi": {"args": "-c %s/deploy/iceoryx/roudi_config.toml"}}}\n' "$PWD" \
  > deploy/config/central/rds.json
theia install svc && theia start -d
theia call RdsCtl GetStatus --data '{}'           # "roudi_up": true
```

The per-machine override reaches the node (despite the `.art` comment calling
that path unreliable). The config used on pascal is
`/bigdata/work/bond001/svc_ws/deploy/iceoryx/roudi_config.toml`: pools of
128 B×10000, 1 KB×5000, 16 KB×1000, 128 KB×200, 1 MB+1 KB×32 and 4 MB×32
(≈217 MB shared memory). A bigger template for cameras:
`perotheia-packages/percept/deploy/roudi-cameras.toml` (64×4 MB).

What a node declares to publish or subscribe:

```
node atomic Cam {
    tipc type=... instance=0
    requires_timers
    requires_rds                 // gen-fc links //services/rds/transport:rds,
                                 // main calls ara::rds::Runtime::Init(<process>)
    rds {
        stream frames { role=writer chunk_size=1048576 history=4 }
    }
}
```

- A stream is keyed on its WRITER: instance `"/<Writer_prototype_capitalized>/<stream>"`,
  e.g. `/Frame_producer/frames` → iceoryx service/instance/event
  `Frame_producer/frames/frame`. The writer uses the generated
  `rds_streams::make_frames_writer()`.
- **A reader names its writer with `from=`** (artheia host-pascal, step 2):
  `stream frames { role=reader from=frame_producer chunk_size=1048576 history=4 }`,
  or `from=cam_rig.cam_front_l` when the writer's stream has another name. The
  generated `rds_streams::make_frames_reader()` then opens the writer's instance.
  Give the reader the writer's chunk_size/history. `from` is the writer's
  PROTOTYPE name (its runtime identity), and `from=` on a writer is a parse
  error. A reader without `from` keeps the old self-keyed instance, which no writer
  publishes. Hand-written `resolve_instance("/<Writer>/<s>", …)` (carla_ws,
  percept, chair-ws) still works unchanged.
- gen-fc now also gives a `requires_rds` node's state target the
  `//services/rds/transport:rds` dep, and seeds its write-once `_state.hh` with
  the include and a handle hint.
- **Per-prototype streams (step 4, artheia 50e0d17 / theia 1ee4091).** A
  writer node prototyped N times publishes N streams, each keyed on its RUNTIME
  prototype name: main.cc calls `<proto>.set_node_name("<proto>")`, and the node
  MEMBERS `make_<s>_writer()` / `make_<s>_reader()` / `<s>_spec()` key on
  `node_name()` via `ara::rds::owner_instance()`. Readers:
  `stream left { role=reader from=cam_left.frames … }` names the writer's
  PROTOTYPE. Use the members; the flat `rds_streams::` functions are keyed at
  generation time (node type in a package lib) and kept only for old callers.
  Demo: `services/rds/demo` (cam_left + cam_right → two streams).
- Size limits: a pool's `size` is the PAYLOAD capacity (iceoryx 1.0 adds a
  40-byte chunk header itself); a loan goes to the smallest pool with
  `size >= loaned bytes`, and the largest pool is the hard ceiling (no
  fragmentation). `history` = chunks kept for late
  joiners; the reader's queue capacity = `history` (4 if 0). The runtime name and
  each id are capped at 100 chars.
- Run: `bazel build //services/rds/demo/... //services/rds/test:rds_roundtrip`;
  with RouDi up, `bazel-bin/services/rds/test/rds_roundtrip` prints
  `rds-roundtrip: OK … producer ptr == reader ptr`; the two
  `services/rds/demo/Rds{Producer,Consumer}/main/rdsdemo` processes log
  `published N` / `received N … bad=0`.

## The host RouDi (one per host; owner bond001)

iceoryx runs ONE RouDi per host user. On pascal it is started once, as axadmin,
from the framework repo's host config:

```sh
nohup iox-roudi -c /home/axadmin/repo/theia/deploy/iceoryx/roudi-host-pascal.toml \
    > /bigdata/work/bond001/tools/roudi.log 2>&1 &
pgrep -a iox-roudi            # is it up?
```

- Pools (payload sizes): 128 B×10000, 1 KiB×5000, 16 KiB×1000, 64 KiB×64,
  128 KiB×64, 512 KiB×128, 1 MiB+1 KiB×144, 8 MiB+64 KiB×144, about 1.47 GB of
  /dev/shm. That covers spr (wc/road, min/full/per-layer, grid + replay with two
  readers) and the CARLA rig (12 cameras: 4 RGB + 4 depth + 4 semantic, at
  640x360 now and 1080p later). The sizing table is in the file's header.
- Everyone attaches to it: just start your stack. **Do not start your own
  iox-roudi** (it fails "Could not acquire lock" while the host one runs). In a
  `--with-services` workspace switch the rds FC's broker off:
  `deploy/config/<machine>/rds.json` = `{"nodes": {"roudi": {"enabled": false}}}`.
- Out of chunks (a writer's Loan() fails / RouDi logs RUNNING_OUT_OF_CHUNKS):
  raise the pool's count in the file, tell bond001, and restart RouDi when no
  RDS process is attached (all writers/readers must restart after a RouDi restart).

## Several stacks on one host

pascal's assignment: bond001 = supervisor instance 0, bond002 = 2, bond003 = 3.
`theia start -d --instance N` runs an ISOLATED stack at supervisor `:N`; `theia
call/cast … --instance N` and `tdb` follow. Since 0f66527 its pg joins and
heartbeats go to its own supervisor (`THEIA_PG_SUPERVISOR_INSTANCE=N`, set by
theia start); before, they went to whichever stack held `:0`. `theia start`
refuses if `0x80020001:N` is already bound: check with `tipc nametable show |
awk '$1==2147614721'`. The scaffold's `test/<pkg>.robot` probe dials the
node's declared instance, so it only passes on instance 0; on `:N` use
`theia call <Node> <Op> --instance N`.

**Re-source `env.sh` in every workspace you `cd` into.** It exports
`THEIA_WORKSPACE` = the directory it was sourced from, and `theia` uses that
over the current directory.

## Tests

Results on pascal, 2026-11-12:

| suite | command | result |
| --- | --- | --- |
| user-flow harness | `./ci/run.sh` (s1..s5, live; needs tipc + `USE_BAZEL_VERSION`) | **all 5 PASS**, 16 Robot tests |
| hermetic selftests | the two `robot --include hermetic --exclude live …` lines of `.github/workflows/selftest.yml` (env `RF_THEIA_WORKSPACE=$PWD PYTHONPATH=$PWD`) | **18/18** and **15/15** |
| artheia pytest | `python -m pytest artheia/ -q` | **176 passed** |
| tools pytest | `python -m pytest tools/ -q` | **25 passed** |
| every `testing/scenarios/**/*.robot`, one suite at a time | stack DOWN, then the stack-needing ones re-run with `svc_ws` UP (+ RouDi) and without `PYTHONPATH` | 39 suites, 17 green; **57 pass / 49 fail / 1 skip** |

With the stack up these live service suites pass: com_cert_agreement, diag_doip
(2), idsm_probe, rds_roundtrip, sm_fsm. Run them from a workspace whose stack is
up (`cd svc_ws && robot <theia>/testing/scenarios/…`), and build the decoder
plugin first (`bazel build //platform/runtime/trace/...`).

**Do not set `PYTHONPATH=<theia root>` for anything that shells out to
`artheia`**: the `artheia/` submodule dir then shadows the package
("cannot import name '__version__' from 'artheia' (unknown location)").

The 49 failures by cause. None is a pascal problem; they are suite rot or need a
different rig:
- 23: retired staging. The suites call `apps/stage_local.sh`, a `fixtures/` dir,
  or expect `$THEIA_ROOT/install/central`, all gone with demo/: executor
  node_supervision (3) + restart_strategies (3), supervision (5) +
  rest_for_one (1), log trace_collection (5) + trace_crash (1), sm_gate (2),
  sm_central (3).
- 7: vucm admission/barrier need a multi-board network ("No route to host").
- 4: missing generated python modules (`sm`, `supervisor_bridge_pb2`, `client`):
  robot_node (1), sm_signal_e2e (1), trace_egress (2).
- 3: `_phase1_archive`, whose keywords were removed (archived on purpose).
- 3: gen_app_chain: template drift (`Log.hh` no longer has `kLogTag`).
- 3: trace_decoder: `libtrace_decoder_system.so` loads but decodes no SmStateMsg.
- 2: hybrid_automata / broadcast_resumes: "Restarted" not seen within 10 s.
- 2: config_migration needs `CM_V1_DIGEST` and a `theia-migrate` on PATH.
- 1: sm_probe_live: `SSHTransport` not implemented.
- 1: autosar_regen "Catalog PDUs Flow Into Interface Decls" (FOO_01 decl missing).
- skip 1: robot_node live (com gRPC at localhost:7700 not reachable without mTLS env).

Outputs: `/bigdata/work/bond001/robot/{hermetic,hermetic-fc,scen,scen2,probe}/`,
harness `ci/.work/robot/` + `/bigdata/work/bond001/ci_run.log`.

## A workspace + a package from scratch (the path for the next agents)

Both verified end to end under `/bigdata/work/bond001/`.

Package (`probe/`), exactly as the scaffold prints it:

```sh
mkdir probe && cd probe && source <theia>/env.sh && export USE_BAZEL_VERSION=9.1.1
theia init --kind package --name probe
source local_setup.sh
artheia gen-fc-lib system/probe/package.art --out src --proto-out proto --ns ara::probe
artheia gen-fc system/probe_tester/component.art --out apps --proto-out proto
artheia gen-manifest system/probe_tester/component.art manifest/probe/manifest.py
theia manifest rig && theia install rig && theia start -d
python -m robot test/probe.robot          # ProbeCtrl Serves CtlIf Over TIPC: PASS
theia stop
```

Workspace (`ws/`) consuming that package as a bazel module:

```sh
mkdir ws && cd ws && source <theia>/env.sh && theia init     # app = system/ws (dir name)
mkdir packages && ln -s ../../probe packages/probe            # (a git submodule in real life)
cat >> MODULE.bazel <<'X'
bazel_dep(name = "probe", version = "0.1.0")
local_path_override(module_name = "probe", path = "packages/probe")
X
ln -s ../packages/probe/system/probe system/probe
# system/ws/component.art: `import system.probe.*`,
#   composition ProbeApp { prototype ProbeCtrl probe },  cluster member `composition ProbeApp probe_app`
artheia gen-fc system/ws/component.art --out apps --proto-out proto   # → @probe//src/lib:probe_lib
artheia gen-manifest system/ws/component.art manifest/ws/manifest.py
theia manifest ws && theia install ws && theia start -d
tdb ps                                     # app + probe_app running
theia call ProbeCtrl Ping --data '{}'      # {"seq":0,"ready":false}: the stub, reachable
theia stop
```

The package's lib must be generated INSIDE the package first (gen-fc-lib above):
its `src/lib` is gitignored codegen, absent from a fresh clone.

## Deviations from the documented commands

- `.bazelversion` 9.1.0 vs installed 9.1.1, and no bazel apt repo on the host:
  `export USE_BAZEL_VERSION=9.1.1`.
- README quick start leaves out `pip install nanopb` and `artheia[probe]`;
  `theia manifest` without a target errors; `demo/` no longer exists (retired
  5d0b3ed; `ci/run.sh` replaced it).
- apt: the CI list misses libacl1-dev, libbpf-dev and iceoryx (CI never builds
  all of `//services/...`).
- `per` needs etcd at 127.0.0.1:2379; none is documented or provided for a dev box.
- RouDi's `.art` default config `/etc/iceoryx/roudi_config.toml` doesn't exist;
  override per machine in `deploy/config/<machine>/rds.json`.
- Older docs: `gen-app --kind fc|package` is a hard-erroring shim; use
  `gen-fc` (app/FC) and `gen-fc-lib` (package). Fixed in artheia README and in
  SKILL.md (`gen-fc --kind package` → `gen-fc-lib`) on host-pascal.
- `contrib/skills/README.md` said `docs/skills/`; the skills are under
  `contrib/skills/` (fixed).
- Bare `theia init` names the app after the directory (`system/<dir>`), not
  `system/apps` as the skill says.
- The package README says a consumer sees `//packages/probe/src/lib:probe_lib`;
  what works (and what gen-fc emits) is module-qualified `@probe//src/lib:probe_lib`.
- The package scaffold ships `local_setup.sh`, the workspace scaffold
  `setup_local.sh`.
- `theia cast` enum values: use the proto label (`HealthLevel_OK`), not the .art one.
- RDS reader instance naming: see the RDS section. The generated reader handle
  is unusable across nodes.
- `services/rds/test/rds_roundtrip` segfaulted under the default `-c opt`
  (side effects inside assert); fixed on host-pascal (8ebb408).
- `services/rds/demo` had only the producer, with no-op bodies; consumer +
  bodies added on host-pascal (9e7e880).
- Fixed in step 3 (theia 0f66527, artheia 026cefc): package scaffolds all got
  TIPC 0x800100F0 (now name-derived 0xD0HHHH01); an added composition never
  reached a pristine executor.py; two enums sharing a value label clashed in
  the C++ aliases; pg/heartbeats under `--instance N` hit supervisor :0;
  `theia start -d --instance N` took N as the machine name.
- This file lives under `/docs/`, which theia's `.gitignore` reserves for a
  separate docs repo: it was force-added. Move it if that clashes.
