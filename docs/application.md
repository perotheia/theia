# Applications

How a vendor app is described in artheia, generated into C++14 scaffold,
built, and connected to the gateway. Companion to
[gateway/LAYERS.md](gateway/LAYERS.md) (which describes everything
*below* the app shim) and [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md)
(the workflow spec the generators read from).

## TL;DR

```
vendor/<name>/system/*.art           # what the app subscribes to
     │  artheia gen-app
     ▼
applications/<name>/                 # three-slice C++ scaffold
  core/   <Node>Inputs.hh            # slice 1: DI bundle (regen-safe)
  app/    <Node>.{hh,cc}             # slice 2: rx loop, dispatch (regen-safe)
          <Node>_main.cc
  app/impl/ <Node>_handlers.cc       # slice 3: user code (write-once)
  CMakeLists.txt
     │  cmake -B build && cmake --build build
     ▼
applications/<name>/build/<name>     # final ELF, talks TIPC to cmp_gw
```

The runtime contract is `platform::runtime::LifecycleInterface`
(`OnCreate` / `OnStart` / `OnStop`, all `noexcept`). The app receives
typed nanopb POD structs (`shared_<Pdu>`, `mlbevo_gen2_<Pdu>`,
`can_<bus>_<Pdu>`) — wire framing and protobuf decoding live in the
generated `rx_loop_`, not in user code.

## The three slices

`artheia gen-app` produces three categories of file with different
lifetimes:

| Slice | Files | Regenerated on every run? | Hand-edit? |
|-------|-------|---------------------------|------------|
| 1 — core | `core/<Node>Inputs.hh` | yes | no |
| 2 — app  | `app/<Node>.{hh,cc}`, `app/<Node>_main.cc`, `CMakeLists.txt` | yes | no |
| 3 — impl | `app/impl/<Node>_handlers.cc` | **first time only** | yes |

Slice 3 is the only place user code goes. The generator checks for the
file's existence and refuses to overwrite it; if you delete it, the
next regen recreates empty `TODO` stubs. All other files carry an
`AUTO-GENERATED ... DO NOT EDIT` banner.

`app/gw_client.{h,cpp}` is currently vendored alongside the generated
sources (a one-time copy from `gateway/demo/pero_cmp_gw_cln_demo`).
It will move to a public header in `gateway/libs/libgw/` later; for
now treat it as part of slice 2.

## The .art source

A node declaration in artheia (textX-based DSL) drives generation. The
generator needs three things from it:

1. **The node name and namespace** — `node atomic OddPathMonitor` in
   `vendor/<name>/system/components/...` gives the C++ class name and
   the project name.
2. **Receiver ports** — one `receiver <port> requires <Iface>` per PDU
   the app subscribes to. Each becomes a callback
   `on_<port>(const GwMessageHeader& hdr, const <CxxStruct>& msg)` in
   `impl/<Node>_handlers.cc`.
3. **TIPC binding** — `tipc type=0xc0010001 instance=0` gives the
   addresses the app uses on the kernel TIPC socket.

The interface name (`ACC_07_Iface`) maps to a PDU name (`ACC_07`)
which the netgraph resolves to a wire address (CAN id or FlexRay
slot/channel) and a protobuf package (`shared`, `mlbevo_gen2`,
`can_<bus>`). Both bits of info come from artefacts the generator
loads at gen time — see the next section.

## Running the generator

```sh
PATH="$PWD/.venv/bin:$PATH" artheia gen-app \
  --vendor-root vendor/odd_path_client \
  --out         applications/odd_path_client \
  --namespace   odd_path_client \
  --project     odd_path_client \
  --netgraph    autosar/mlbevo_gen2_cmp_psp/system/kcan/netgraph.json \
  --netgraph    autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/netgraph.json \
  --psp-proto-root bazel-bin/autosar/mlbevo_gen2_cmp_psp/codec_proto
```

Flag-by-flag:

- `--vendor-root` — directory holding the `.art` system definition.
- `--out` — where to drop the generated scaffold (this is the project
  CMake builds from).
- `--namespace` — C++ namespace wrapping every generated symbol.
- `--project` — CMake project name and ELF name.
- `--netgraph` — one or more `netgraph.json` files (one per bus the
  app subscribes to). Each maps `pdu_name` → `bus_kind` +
  `{can_id}` or `{slot_id, channel_idx}`. The generated `rx_loop_`
  uses these literals to filter incoming frames by header.
- `--psp-proto-root` — directory of `.proto` files produced by the PSP
  build. The generator parses `package` declarations to learn the C
  struct prefix (`shared` vs `mlbevo_gen2` vs `can_<bus>`) and the
  `<dir>/<Pdu>.pb.h` include path.

The PSP build must run first so the `.proto` files exist:

```sh
bazel build //autosar/mlbevo_gen2_cmp_psp:psp_protos
bazel build //autosar/mlbevo_gen2_cmp_psp:psp_nanopb   # the .pb.h/.pb.c
```

## The lifecycle

`platform::runtime::LifecycleInterface` (`platform/runtime/include/LifecycleInterface.hh`):

```cpp
class LifecycleInterface {
 public:
  virtual void OnCreate() noexcept = 0;
  virtual void OnStart()  noexcept = 0;
  virtual void OnStop()   noexcept = 0;
};
```

Three hooks, all `noexcept` (Adaptive AUTOSAR-style — the call sites
stay lockfree). The generated app implements them:

- **`OnCreate`** — constructs `GwClient` and calls `connect()`. If
  connect fails it logs and returns; `OnStart` then refuses to start.
- **`OnStart`** — spawns `rx_thread_` running `rx_loop_()`. Returns
  immediately; the app is now live.
- **`OnStop`** — clears `running_`, joins the thread, calls
  `client_->disconnect()`.

`<Node>_main.cc` orchestrates: build runtime context, instantiate the
app, drive `OnCreate → OnStart → wait(SIGINT|SIGTERM) → OnStop`. The
sleep loop is intentionally dumb — it just keeps `main` alive while
the rx thread does the real work.

## What the rx loop does

The generated `rx_loop_()` in `app/<Node>.cc` is the dispatch core:

```cpp
GwMessageHeader hdr{};
uint8_t         proto[256];
while (running_.load()) {
  ssize_t n = client_->recv_signal(&hdr, proto, sizeof(proto), 200);
  if (n <= 0) continue;

  // one if-branch per receiver port:
  if (hdr.bus_type == GW_BUS_TYPE_CAN && hdr.can.can_id == 0x12eu) {
    shared_ACC_07 msg = shared_ACC_07_init_zero;
    pb_istream_t  stream = pb_istream_from_buffer(proto, hdr.proto_len);
    if (pb_decode(&stream, shared_ACC_07_fields, &msg)) {
      this->on_acc_07(hdr, msg);
    }
    continue;
  }
  // ... more branches, then drop silently if unmatched
}
```

The filter values (`0x12eu`, slot ids, channel indices) are baked in
at generation time from the netgraph; the proto package prefix
(`shared_`, `mlbevo_gen2_`, `can_<bus>_`) and the `<dir>/<Pdu>.pb.h`
include path are baked in from `--psp-proto-root`.

`recv_signal` blocks up to 200ms so the loop can shut down promptly
when `running_` goes false.

## Writing handlers

User code lives in `app/impl/<Node>_handlers.cc`. The generator
creates one empty `TODO` stub per receiver port; you fill them in.

```cpp
void OddPathMonitor::on_acc_07(
    const GwMessageHeader& hdr,
    const shared_ACC_07& msg) noexcept {
  static std::atomic<uint64_t> count{0};
  uint64_t n = count.fetch_add(1) + 1;
  if (n == 1 || (n % 100) == 0) {
    std::fprintf(stderr,
        "[ACC_07 #%lu seq=%u] CRC=%u Anhalteweg=%.2f Folgebeschl=%.3f\n",
        (unsigned long)n,
        (unsigned)hdr.tipc.sequence_num,
        (unsigned)msg.ACC_07_CRC,
        (double)msg.ACC_Anhalteweg,
        (double)msg.ACC_Folgebeschl);
  }
}
```

The handler signature is `noexcept` — handlers must not throw. The
`hdr` carries TIPC sequence and timestamp; `msg` is a plain POD with
one field per signal in the PDU. Field names match the FIBEX/DBC
signal names.

Handlers run on `rx_thread_`. If you need to do anything blocking,
hand the work to another thread or queue; long handlers stall the
gateway-to-app pipeline.

## Inputs (dependency injection)

`core/<Node>Inputs.hh` is the DI bundle the runtime passes to the
constructor:

```cpp
class OddPathMonitorInputs {
 public:
  std::shared_ptr<platform::runtime::TimerFactoryInterface> timer_factory_;
  odd_path_monitor_signals&                                 signals_;
  std::shared_ptr<platform::runtime::Clock>                 clock_;
  std::shared_ptr<platform::runtime::Logger>                logger_;
};
```

The runtime context (`Clock`, `Logger`, `TimerFactory`) is built in
`<Node>_main.cc` from `platform/runtime/`. The
`<node_snake>_signals` struct is a forward-declared opaque type; today
it is a placeholder, reserved for future codegen that wires multiple
apps to one libgw process.

## Building

After regenerating:

```sh
cd applications/odd_path_client
cmake -B build -G Ninja
cmake --build build
```

The CMakeLists drives:

1. **PSP nanopb static lib** — `file(GLOB_RECURSE)` of every `.pb.c`
   under `PSP_PROTO_INC`, wrapped in a `STATIC` library. CMake
   silently drops out-of-tree sources from `add_executable` in some
   versions; the explicit library sidesteps that.
2. **Main executable** — generated `<Node>.cc` / `<Node>_main.cc` /
   `impl/<Node>_handlers.cc` + the vendored `gw_client.cpp`.
3. **Link** — main exe links the protos archive with
   `-Wl,--whole-archive` (so all `<pdu>_msg` symbols stay; each is
   referenced through a `<pdu>_fields` macro), then
   `platform/runtime`, `libgw`, `libcmpdecoder`,
   `libprotobuf-nanopb.a`, `pthread`.

CMake cache variables (override on the command line if needed):

- `WORKSPACE_ROOT` — parent of `applications/`, `platform/`,
  `gateway/`, `autosar/`. Defaults to `${CMAKE_SOURCE_DIR}/../..`.
- `PLATFORM_RUNTIME`, `LIBGW`, `BAZEL_BIN`, `PSP_PROTO_INC` — derived
  from `WORKSPACE_ROOT`; rarely overridden directly.

## Running end-to-end

The app needs the kernel TIPC module loaded and `cmp_gw` running.
Pcap replay is the easiest way to drive it without real hardware:

```sh
sudo modprobe tipc          # one-time, host-level
bazel build //services/pero_cmp_gw_svc:cmp_gw

# Terminal 1: cmp_gw replays a pcap into the TIPC fabric.
./bazel-bin/services/pero_cmp_gw_svc/cmp_gw \
    --pcap up/acc07_20s.pcap --loop \
    169.254.8.3 ./bazel-bin/autosar/mlbevo_gen2_cmp_psp

# Terminal 2: the app subscribes via TIPC and runs its rx loop.
./applications/odd_path_client/build/odd_path_client
```

Expected output on the app side (every 100th ACC_07 frame):

```
[INFO ] OddPathMonitor: starting
[INFO ] OddPathMonitor: rx loop started
[ACC_07 #1 seq=1463] CRC=0 Anhalteweg=20.46 Folgebeschl=3.020
[ACC_07 #100 seq=1562] CRC=0 Anhalteweg=20.46 Folgebeschl=3.020
...
```

Without `--loop` the pcap replays once and `cmp_gw` exits cleanly;
with `--loop` it restarts from the top forever.

## Regenerating after a model change

`gen-app` is idempotent except for slice 3:

- Edit `vendor/<name>/system/...` (or rebuild the PSP if PDU layouts
  changed), then re-run `artheia gen-app` with the same arguments.
- Slices 1 and 2 (and `CMakeLists.txt`) are overwritten.
- `app/impl/<Node>_handlers.cc` is preserved. If you added a new
  `receiver` port to the `.art` file, the corresponding `on_<port>`
  declaration appears in the generated `<Node>.hh`, but there is no
  stub body in `impl/`. The next compile will fail with an undefined
  reference until you add a method body by hand.
- Removing a port leaves a stale stub in `impl/`; delete it manually.

## Pipeline this fits into

The app sits at the top of the [7-layer pipeline](gateway/LAYERS.md):

```
Hercules ──ASAM-CMP/UDP──▶ cmp_gw ──libcmpdecoder──▶ PSP codec
                                                          │
                                                          ▼
                       GwMessageHeader + nanopb bytes ──TIPC──▶ app::rx_loop_
                                                                       │
                                                  pb_decode <Pdu>      │
                                                                       ▼
                                              on_<port>(hdr, msg)
```

The app only sees the bottom two arrows. Everything above is the
gateway's job and is documented under
[docs/gateway/](gateway/).
