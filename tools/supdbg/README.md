# supdbg — text-mode supervisor debugger

Modeled (loosely) on Erlang/OTP's `dbg` module: a thin Python gRPC
client over the supervisor's existing observation + control surface
(`services/com` SupervisorView). Use it for live debugging and as a
library to drive tests.

## Quick start

```bash
# 1. Generate stubs (one-time, re-run if .proto changes).
./tools/supdbg/gen_protos.sh

# 2. Use it.
PYTHONPATH=tools .venv/bin/python -m supdbg tree
PYTHONPATH=tools .venv/bin/python -m supdbg --target 127.0.0.1:7701 tree
PYTHONPATH=tools .venv/bin/python -m supdbg restart demo_p1
PYTHONPATH=tools .venv/bin/python -m supdbg watch --only event
PYTHONPATH=tools .venv/bin/python -m supdbg                  # interactive REPL
```

## One-shot subcommands

All accept `--target host:port` (default `127.0.0.1:7700`):

| cmd | purpose | exit code |
|---|---|---|
| `tree` | print current TreeSnapshot as a table | 0 on snapshot, 1 on timeout |
| `tree --json` | emit JSON for shell-driven assertions | same |
| `restart <child>` | RestartChild RPC | 0 on success, 1 on supervisor error |
| `terminate <child>` | TerminateChild RPC | same |
| `delete <child>` | DeleteChild RPC | same |
| `watch` | stream events + health + snapshots forever | 0 on Ctrl-C |
| `watch --only event` | filter to one kind (repeatable) | same |
| `wait --kind EXITED --child p1 --timeout 5` | block for matching event; print and exit | 0 found, 2 timeout |
| `repl` | dbg-style interactive shell | 0 on quit |

Friendly errors: unreachable bridges print `cannot reach gRPC bridge
at …` and exit 3, not a Python traceback.

## REPL letter commands (dbg-style)

```
supdbg> h
i                  tree (current snapshot)
r <name>           restart child
t <name>           terminate child
s <name> <cmd...>  start child with start_cmd
d <name>           delete child spec
w                  toggle event+health tail
wt                 toggle tree-snapshot tail (noisy)
n <host:port>      switch target machine
ls                 show current target
h, ?               help
q, exit            quit
```

The tail thread keeps printing observations while you type. Same
behavior as Erlang's `dbg:p/2` once you've turned on tracing.

## Library usage (drive from tests)

```python
import sys; sys.path.insert(0, "tools")
from supdbg import Client, EventKind

with Client("127.0.0.1:7700") as c:
    # Get the current tree.
    snap = c.tree(timeout=3)
    assert any(ch.name == "demo_p1" for ch in snap.children)

    # Cause a restart, wait for the corresponding event.
    c.restart_child("demo_p1")
    ev = c.wait_event(kind=EventKind.RESTARTED, child_name="demo_p1",
                      timeout=10)
    assert ev is not None
```

`Client` is a thin wrapper around the generated gRPC stub:
* `tree(timeout)` — next TreeSnapshot
* `subscribe()` — generator over `Observation` (event / health / snapshot)
* `start_child(spec) / restart_child(name) / terminate_child(name) / delete_child(name)` — unary control RPCs
* `wait_event(kind, child_name, timeout)` — block until a matching SupervisionEvent

## How the wire path looks

```
+----------+  gRPC          +-------------------+   AF_TIPC      +------------+
|  supdbg  | ─────────────▶ │  services/com     │ ─────────────▶ │ supervisor │
| (python) │  port 7700/01  │  (C++ bridge)     │   localhost    │  (C++)     │
+----------+                +-------------------+                +------------+
```

* In `docker-compose`, central exposes `7700:7700` and compute
  `7701:7700` (services-com hardcodes its container-side listen on
  `:7700`).
* On a real target board, point `--target` at the device's IP +
  whatever port `services/com` was started with.

## Regenerating stubs

`./tools/supdbg/gen_protos.sh` runs `grpc_tools.protoc` against
`services/com/proto/` + `platform/supervisor/generated/proto/` and
drops `*_pb2.py` + `*_pb2_grpc.py` under `tools/supdbg/_gen/`. Re-run
after any `.art` edit that changes a supervisor message type
(artheia regenerates the .protos as part of its build).

## What it deliberately doesn't do

* No trace-points (Erlang's `dbg:tp/2` / `dbg:tpl/2`). Our tracer
  is a separate binary (`services/log`, not yet wired); when it
  lands, supdbg will get a `tp` command that subscribes to its TCP
  feed.
* No multi-machine fan-out (you switch targets with REPL's `n`).
  A future `--machines machines.yaml` mode is straightforward but
  out of scope for v1.
