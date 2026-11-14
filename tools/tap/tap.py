#!/usr/bin/env python3
"""theia tap: record and replay any TIPC interface and any RDS stream (INTEGRATION.md 4).

Every integration run records; every component test replays. A recording is a
DIRECTORY, in the shape spr already uses (SPR.md 8, spr_rig.hh):

  meta.yaml                 version, host, instance, start, what was recorded
  <wire_type>.jsonl         one TIPC interface (pg group), one line per message:
                              {"rx_wall_ns": R, "tx_ns": T, "msg": {...}, "pb": "<base64>"}
                            rx_wall_ns = CLOCK_REALTIME when the tap received it;
                            tx_ns = the sender's header stamp; msg = the decoded
                            fields (enums by name, bytes base64); pb = the exact
                            wire bytes.
  streams.txt, <file>.bin   the RDS streams, raw chunks (tools/tap/tap_rds.cc):
                            "<file> <stream>" lines; {rx_wall_ns, bytes, 0} + chunk.

A TIPC interface is a pg GROUP, named by its message's wire type (the group
identity the supervisor allocates on): give the interface (`EgoStateFeed`), the
message (`EgoState`) or the wire type (`system_contracts_EgoState`). Names resolve
against the workspace's system/ tree ($THEIA_WORKSPACE, or --ws).

  theia tap record <dir> [--tipc IF[,IF...]] [--rds STREAM[,STREAM...]] [--seconds S] [--instance N]
  theia tap replay <dir> [--speed X] [--only NAME[,NAME...]] [--start-delay-ms D] [--instance N]
  theia tap info   <dir>

record: joins each group as a member (the supervisor of --instance N, default the
  environment's THEIA_PG_SUPERVISOR_INSTANCE, else 0) and runs tap_rds for the
  streams, until Ctrl-C or --seconds.
replay: publishes every message on its group (TIPC name-sequence multicast to all
  members) and every chunk on its stream, at start + (rx_wall_ns - first) / speed,
  one clock for both halves. Messages go out as recorded (`pb`); a line with only
  `msg` (hand-written, synthetic) is encoded from it. Stamps inside messages are
  the recorded ones. Nothing else may publish the replayed interfaces meanwhile.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

THEIA_ROOT = Path(os.environ.get("THEIA_ROOT") or Path(__file__).resolve().parents[2]).resolve()
sys.path.insert(0, str(THEIA_ROOT / "artheia"))

VERSION = 1
_PROBE_TYPE = 0x8FFE0001          # a synthetic probe identity, outside every allocated range


# ---- name resolution (textual: fast, no textX parse) ---------------------------
@dataclass
class Wire:
    flat: str            # system_contracts_EgoState (the group name, the codec key)
    art_package: str     # system.contracts
    name: str            # EgoState
    proto_root: Path     # where the package's .proto tree lives
    interface: str = ""  # EgoStateFeed, when resolved from one


_PKG_RE = re.compile(r"^\s*package\s+([\w.]+)", re.M)
_MSG_RE = re.compile(r"^\s*message\s+(\w+)\s*\{", re.M)
_SR_RE = re.compile(r"(extern\s+)?interface\s+senderReceiver\s+(\w+)\s*\{([^}]*)\}", re.S)
_DATA_RE = re.compile(r"\bdata\s+([\w.]+)\s+\w+")


def _strip_comments(src: str) -> str:
    return re.sub(r"//[^\n]*", "", src)


def _flat_of(pkg: str, name: str) -> str:
    from artheia.generators.proto import _proto_package_name
    return _proto_package_name(pkg).replace(".", "_") + "_" + name


class Index:
    """Every package.art under <ws>/system (symlinks followed) and the framework's."""

    def __init__(self, ws: Path):
        self.msgs: dict[str, list[Wire]] = {}         # name -> candidates
        self.ifaces: dict[str, list[tuple[str, str]]] = {}   # iface -> [(pkg, data type)]
        seen: set[Path] = set()
        for root in (ws / "system", THEIA_ROOT / "system"):
            if not root.is_dir():
                continue
            for dirpath, _dirs, files in os.walk(root, followlinks=True):
                if "package.art" not in files:
                    continue
                f = Path(dirpath) / "package.art"
                real = f.resolve()
                if real in seen:
                    continue
                seen.add(real)
                self._add(real)

    def _add(self, f: Path) -> None:
        src = _strip_comments(f.read_text(errors="replace"))
        m = _PKG_RE.search(src)
        if not m:
            return
        pkg = m.group(1)
        # <root>/system/<a>/<b>/package.art for package system.a.b -> <root>
        root = f.parent
        for _ in pkg.split("."):
            root = root.parent
        proto_root = root / "proto" if (root / "proto").is_dir() else THEIA_ROOT / "platform" / "proto"
        if root == THEIA_ROOT:
            proto_root = THEIA_ROOT / "platform" / "proto"
        for name in _MSG_RE.findall(src):
            self.msgs.setdefault(name, []).append(
                Wire(_flat_of(pkg, name), pkg, name, proto_root))
        for ext, iname, body in _SR_RE.findall(src):
            if ext:
                continue                        # a forward declaration of another package's
            for t in _DATA_RE.findall(body):
                self.ifaces.setdefault(iname, []).append((pkg, t))

    def resolve(self, spec: str) -> "list[Wire]":
        """An interface -> one Wire per data element (each is its own pg group);
        a message or a wire type -> that one. `<package>.<Interface>` picks one of
        two same-named interfaces."""
        want_pkg = ""
        if "." in spec and spec.rsplit(".", 1)[1] in self.ifaces:
            want_pkg, spec = spec.rsplit(".", 1)
        if spec in self.ifaces:
            by_pkg: dict[str, list[str]] = {}
            for pkg, t in self.ifaces[spec]:
                if not want_pkg or pkg == want_pkg:
                    by_pkg.setdefault(pkg, []).append(t)
            if not by_pkg:
                raise SystemExit(f"tap: no interface {spec} in package {want_pkg}")
            if len(by_pkg) > 1:
                raise SystemExit(f"tap: interface {spec} is declared in {len(by_pkg)} packages: "
                                 f"give one of {', '.join(p + '.' + spec for p in sorted(by_pkg))}")
            pkg, types = next(iter(by_pkg.items()))
            out = []
            for t in types:
                pref = pkg if "." not in t else ".".join(t.split(".")[:-1])
                w = self._msg(t.split(".")[-1], prefer_pkg=pref)
                out.append(Wire(w.flat, w.art_package, w.name, w.proto_root, interface=spec))
            return out
        if spec in self.msgs:
            return [self._msg(spec)]
        for ws in self.msgs.values():          # a wire type
            for w in ws:
                if w.flat == spec:
                    return [w]
        raise SystemExit(f"tap: {spec!r} is not an interface, a message or a wire type "
                         f"in the workspace's system/ tree")

    def _msg(self, name: str, prefer_pkg: str = "") -> Wire:
        cands = self.msgs.get(name, [])
        if not cands:
            raise SystemExit(f"tap: message {name} not found")
        pref = [w for w in cands if w.art_package == prefer_pkg]
        if pref:
            return pref[0]
        if len(cands) > 1:
            raise SystemExit(f"tap: message {name} is in {len(cands)} packages "
                             f"({', '.join(w.flat for w in cands)}): give the wire type")
        return cands[0]


# ---- protobuf <-> json ------------------------------------------------------------
_codecs: dict[str, object] = {}


def _cls(w: Wire):
    from artheia.gen_server.probe.codec import Codec
    c = _codecs.get(str(w.proto_root))
    if c is None:
        c = _codecs[str(w.proto_root)] = Codec(w.proto_root)
    return c._message_class(w.art_package, w.flat)


def to_json(msg) -> dict:
    from google.protobuf.descriptor import FieldDescriptor as FD
    out = {}
    for f in msg.DESCRIPTOR.fields:
        v = getattr(msg, f.name)

        def one(x):
            if f.type == FD.TYPE_MESSAGE:
                return to_json(x)
            if f.type == FD.TYPE_ENUM:
                ev = f.enum_type.values_by_number.get(int(x))
                return ev.name if ev else int(x)
            if f.type == FD.TYPE_BYTES:
                return base64.b64encode(x).decode()
            return x

        is_rep = getattr(f, "is_repeated", None)
        is_rep = is_rep if isinstance(is_rep, bool) else f.label == FD.LABEL_REPEATED
        out[f.name] = [one(x) for x in v] if is_rep else one(v)
    return out


def from_json(w: Wire, d: dict) -> bytes:
    from google.protobuf import json_format
    m = _cls(w)()
    json_format.ParseDict(d, m, ignore_unknown_fields=False)
    return m.SerializeToString()



# ---- recorded / expected ---------------------------------------------------
_COUNTERS = ("seq", "sequence", "tick", "frame")   # a message's OWN counter first (Trajectory.tick = the EgoState it answers)


def _counter(m) -> "tuple | None":
    """(run, value) of the message's first counter field, or None (no counter)."""
    names = {f.name for f in m.DESCRIPTOR.fields}
    for c in _COUNTERS:
        if c in names:
            run = getattr(m, "run", 0) if "run" in names else 0
            return (int(run), (c, int(getattr(m, c))))
    return None


def _expected(seen: dict) -> dict:
    out = {}
    for flat, runs in seen.items():
        rec = exp = 0
        counter = ""
        for run, vals in runs.items():
            nums = sorted(v for _, v in vals)
            counter = next(iter(vals))[0]
            rec += len(nums)
            exp += nums[-1] - nums[0] + 1
        out[flat] = {"counter": counter, "recorded": rec, "expected": exp, "missing": exp - rec}
    return out

# ---- the probe / pg plumbing --------------------------------------------------------
def _sup_instance(arg: "int | None") -> int:
    if arg is not None:
        return arg
    try:
        return int(os.environ.get("THEIA_PG_SUPERVISOR_INSTANCE", "") or 0)
    except ValueError:
        return 0


def _pg(instance: int, name: str):
    from artheia.gen_server.probe import ArtheiaContext
    from artheia.gen_server.probe.pg import PgProbe
    ctx = ArtheiaContext(str(THEIA_ROOT / "system/supervisor/component.art"),
                         proto_root=str(THEIA_ROOT / "platform/proto"))
    probe = ctx.probe_external(_PROBE_TYPE, (os.getpid() & 0xFFFF) | (instance << 16),
                               name=name).start()
    return probe, PgProbe(probe, node_name=name, sup_instance_=instance)


def _rds_env(inst: int) -> dict:
    """tap_rds follows the stack's naming rule (ara::rds::stack_qualified: streams of
    stack N != 0 are "<Service>@N"), so it gets the stack's instance, flag or env."""
    env = dict(os.environ)
    env["THEIA_PG_SUPERVISOR_INSTANCE"] = str(inst)
    return env


def _tap_rds() -> Path:
    b = THEIA_ROOT / "bazel-bin/tools/tap/tap_rds"
    if not b.is_file():
        raise SystemExit(f"tap: {b} not built: (cd {THEIA_ROOT} && bazel build //tools/tap:tap_rds)")
    return b


def _write_meta(d: Path, meta: dict) -> None:
    import yaml
    (d / "meta.yaml").write_text(yaml.safe_dump(meta, sort_keys=False))


def _read_meta(d: Path) -> dict:
    import yaml
    p = d / "meta.yaml"
    return (yaml.safe_load(p.read_text()) or {}) if p.is_file() else {}


def _csv(v: "str | None") -> list[str]:
    return [x for x in (v or "").split(",") if x]


# ---- record ---------------------------------------------------------------------------
def cmd_record(a) -> int:
    d = Path(a.dir)
    d.mkdir(parents=True, exist_ok=True)
    ws = Path(a.ws or os.environ.get("THEIA_WORKSPACE") or os.getcwd())
    inst = _sup_instance(a.instance)
    wires: list[Wire] = []
    if a.tipc:
        idx = Index(ws)
        for spec in _csv(a.tipc):
            for w in idx.resolve(spec):
                if w.flat not in {x.flat for x in wires}:
                    wires.append(w)
    streams = _csv(a.rds)
    if not wires and not streams:
        raise SystemExit("tap record: nothing to record (--tipc and/or --rds)")

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    rds = None
    if streams:
        cmd = [str(_tap_rds()), "record", str(d), ",".join(streams)]
        rds = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, env=_rds_env(inst))

    counts: dict[str, int] = {}
    seen: dict[str, dict] = {}      # flat -> run -> the counter values received
    files = {}
    lock = threading.Lock()
    probe = pg = None
    joins: list = []
    if wires:
        probe, pg = _pg(inst, f"tap-rec-{os.getpid()}")
        pg.start_keepalive()
        from artheia.gen_server.probe import wire as W
        for w in wires:
            files[w.flat] = open(d / f"{w.flat}.jsonl", "a", buffering=1)
            counts[w.flat] = 0
            sid = W.service_id(w.flat)
            cls = _cls(w)

            def on_frame(fields, w=w, sid=sid, cls=cls):
                rx = time.time_ns()
                raw = fields.get("_raw")
                if raw is None or fields.get("_service_id") != sid:
                    return
                m = cls()
                m.ParseFromString(raw)
                line = json.dumps({"rx_wall_ns": rx, "tx_ns": fields.get("_tx_ns", 0),
                                   "msg": to_json(m), "pb": base64.b64encode(raw).decode()})
                key = _counter(m)
                with lock:
                    files[w.flat].write(line + "\n")
                    counts[w.flat] += 1
                    if key is not None:
                        seen.setdefault(w.flat, {}).setdefault(key[0], set()).add(key[1])

            joins.append((w.flat, on_frame))

    # Join each group with RETRIES and backoff, in the background: a supervisor busy
    # starting its stack can let a PgJoin call time out (2 s x 5); that must not end the
    # recording (final run 1, 2026-11-13). Groups that joined record at once; one that
    # never joins within --join-timeout is reported in the summary, not fatal.
    join_failed: dict[str, str] = {}

    def join_all():
        pending = list(joins)
        deadline = time.monotonic() + a.join_timeout
        delay = 0.5
        while pending and not stop.is_set():
            left = []
            for flat, cb in pending:
                try:
                    rep = pg.join(flat, on_cast=cb)
                except Exception as e:                    # TimeoutError from call_addr
                    left.append((flat, cb)); join_failed[flat] = f"{type(e).__name__}: {e}"
                    continue
                if int(rep.get("status", 1)) != 0:
                    left.append((flat, cb)); join_failed[flat] = f"refused: {rep}"
                    continue
                join_failed.pop(flat, None)
                print(f"tap: joined {flat} (group 0x{int(rep['group_type']):08x}, member {rep['instance']})",
                      file=sys.stderr)
            pending = left
            if pending:
                if time.monotonic() >= deadline:
                    for flat, _ in pending:
                        print(f"tap: GAVE UP joining {flat} after {a.join_timeout:.0f} s: "
                              f"{join_failed.get(flat)}", file=sys.stderr)
                    return
                print(f"tap: {len(pending)} group(s) not joined yet (supervisor :{inst} busy?), "
                      f"retry in {delay:.1f} s", file=sys.stderr)
                stop.wait(delay)
                delay = min(delay * 2, 5.0)

    joiner = None

    meta = _read_meta(d)
    meta.update({
        "version": VERSION, "host": socket.gethostname(), "instance": inst,
        "workspace": str(ws), "started_wall_ns": time.time_ns(),
        "tipc": sorted({*(meta.get("tipc") or []), *[w.flat for w in wires]}),
        "interfaces": {**(meta.get("interfaces") or {}),
                       **{w.flat: {"interface": w.interface, "message": w.name,
                                   "art_package": w.art_package} for w in wires}},
        "rds": sorted({*(meta.get("rds") or []), *streams}),
    })
    _write_meta(d, meta)
    if joins:
        joiner = threading.Thread(target=join_all, daemon=True)
        joiner.start()
    print(f"tap: recording -> {d} (Ctrl-C to stop)", file=sys.stderr)
    t_end = time.monotonic() + a.seconds if a.seconds else None
    while not stop.is_set():
        if t_end and time.monotonic() >= t_end:
            break
        if rds and rds.poll() is not None:
            print("tap: tap_rds exited early", file=sys.stderr)
            break
        stop.wait(0.1)

    stop.set()
    if joiner:
        joiner.join(timeout=3)
    summary = {"mode": "record", "dir": str(d), "tipc": dict(counts)}
    # RECORDED vs EXPECTED per feed, from its counter (tick / seq / sequence / frame, per run):
    # the span the producer emitted while we listened. A gap = casts this member missed (a
    # shared member name, a congested socket): warned, never silent.
    exp = _expected(seen)
    if exp:
        summary["expected"] = exp
        for flat, e in exp.items():
            if e["missing"]:
                print(f"tap: WARNING {flat}: recorded {e['recorded']} of {e['expected']} expected "
                      f"(by {e['counter']}), {e['missing']} missing", file=sys.stderr)
    if join_failed:
        summary["not_joined"] = dict(join_failed)
    if pg:
        pg.shutdown()
        probe.stop()
    for f in files.values():
        f.close()
    rc = 0
    if rds:
        if rds.poll() is None:
            rds.send_signal(signal.SIGTERM)
        out, _ = rds.communicate(timeout=10)
        rc = rds.returncode
        try:
            summary["rds"] = json.loads(out.strip().splitlines()[-1])["streams"]
        except Exception:
            summary["rds"] = out.strip()
    print(json.dumps(summary))
    return rc


# ---- replay ---------------------------------------------------------------------------
def _load_tipc(d: Path, ws: Path, only: list[str]) -> list[tuple[int, Wire, bytes]]:
    meta = _read_meta(d)
    idx = None
    events = []
    for f in sorted(d.glob("*.jsonl")):
        flat = f.stem
        info = (meta.get("interfaces") or {}).get(flat, {})
        if only and flat not in only and info.get("interface") not in only and info.get("message") not in only:
            continue
        w = None
        if info.get("art_package"):
            idx = idx or Index(ws)
            for cand in idx.msgs.get(info.get("message", ""), []):
                if cand.flat == flat:
                    w = cand
        if w is None:
            idx = idx or Index(ws)
            try:
                w = idx.resolve(flat)[0]
            except SystemExit:
                print(f"tap: skip {f.name}: {flat} not known in {ws}", file=sys.stderr)
                continue
        for n, line in enumerate(f.read_text().splitlines(), 1):
            if not line.strip():
                continue
            rec = json.loads(line)
            pb = base64.b64decode(rec["pb"]) if rec.get("pb") else from_json(w, rec.get("msg") or {})
            events.append((int(rec["rx_wall_ns"]), w, pb))
    events.sort(key=lambda e: e[0])
    return events


def _rds_first_rx(d: Path, only: list[str]) -> "int | None":
    p = d / "streams.txt"
    if not p.is_file():
        return None
    first = None
    for line in p.read_text().splitlines():
        parts = line.split()
        if len(parts) != 2 or (only and parts[1] not in only):
            continue
        b = d / parts[0]
        if b.is_file() and b.stat().st_size >= 16:
            with open(b, "rb") as fh:
                rx = struct.unpack("<Q", fh.read(8))[0]
            first = rx if first is None else min(first, rx)
    return first


def cmd_replay(a) -> int:
    d = Path(a.dir)
    ws = Path(a.ws or os.environ.get("THEIA_WORKSPACE") or os.getcwd())
    inst = _sup_instance(a.instance)
    only = _csv(a.only)
    rds_only = [x for x in only if x.startswith("/")]
    tipc_only = [x for x in only if not x.startswith("/")]
    events = [] if (only and not tipc_only) else _load_tipc(d, ws, tipc_only)
    rds_rx0 = None if (only and not rds_only) else _rds_first_rx(d, rds_only)
    if not events and rds_rx0 is None:
        raise SystemExit(f"tap replay: nothing to replay in {d}")
    rx0 = min([e[0] for e in events[:1]] + ([rds_rx0] if rds_rx0 is not None else []))

    from artheia.gen_server.probe import wire as W
    probe = pg = None
    gtypes: dict[str, int] = {}
    if events:
        probe, pg = _pg(inst, f"tap-play-{os.getpid()}")
        for flat in {e[1].flat for e in events}:
            rep = pg._sup_join_call(flat, join=False)      # resolve: the group's TIPC type
            if int(rep.get("status", 1)) != 0 or not int(rep.get("group_type", 0)):
                raise SystemExit(f"tap replay: cannot resolve group {flat} on supervisor :{inst}: {rep}")
            gtypes[flat] = int(rep["group_type"])

    start = time.time_ns() + a.start_delay_ms * 1_000_000
    rds = None
    if rds_rx0 is not None:
        cmd = [str(_tap_rds()), "replay", str(d), "--speed", str(a.speed),
               "--start-wall-ns", str(start), "--rx0-ns", str(rx0)]
        if rds_only:
            cmd += ["--only", ",".join(rds_only)]
        rds = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, env=_rds_env(inst))

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    sock = socket.socket(socket.AF_TIPC, socket.SOCK_RDM)
    sent: dict[str, int] = {}
    late_max = 0
    for rx, w, pb in events:
        due = start + int((rx - rx0) / a.speed)
        while not stop.is_set():
            now = time.time_ns()
            if now >= due:
                break
            stop.wait(min((due - now) / 1e9, 0.05))
        if stop.is_set():
            break
        hdr = W.Header(msg_type=W.MSG_GEN_CAST, proto_len=len(pb),
                       service_id=W.service_id(w.flat), timestamp_ns=time.time_ns())
        try:
            sock.sendto(hdr.pack() + pb, (socket.TIPC_ADDR_NAMESEQ, gtypes[w.flat], 0, 0xFFFFFFFF))
        except OSError as e:
            print(f"tap: send {w.flat}: {e}", file=sys.stderr)
            continue
        sent[w.flat] = sent.get(w.flat, 0) + 1
        late_max = max(late_max, time.time_ns() - due)
    sock.close()
    if probe:
        probe.stop()
    summary = {"mode": "replay", "dir": str(d), "speed": a.speed, "tipc": sent,
               "tipc_late_max_ms": round(late_max / 1e6, 3)}
    rc = 0
    if rds:
        if stop.is_set() and rds.poll() is None:
            rds.send_signal(signal.SIGTERM)
        out, _ = rds.communicate()
        rc = rds.returncode
        try:
            r = json.loads(out.strip().splitlines()[-1])
            summary["rds"], summary["rds_late_max_ms"] = r["streams"], r["late_max_ms"]
        except Exception:
            summary["rds"] = out.strip()
    print(json.dumps(summary))
    return rc


# ---- info ------------------------------------------------------------------------------
def cmd_info(a) -> int:
    d = Path(a.dir)
    meta = _read_meta(d)
    out = {"dir": str(d), "meta": meta, "tipc": {}, "rds": {}}
    for f in sorted(d.glob("*.jsonl")):
        rx = [json.loads(l)["rx_wall_ns"] for l in f.read_text().splitlines() if l.strip()]
        out["tipc"][f.stem] = {"messages": len(rx),
                               "seconds": round((rx[-1] - rx[0]) / 1e9, 3) if len(rx) > 1 else 0}
    p = d / "streams.txt"
    if p.is_file():
        for line in p.read_text().splitlines():
            parts = line.split()
            if len(parts) != 2:
                continue
            n = size = 0
            first = last = None
            with open(d / parts[0], "rb") as fh:
                while True:
                    h = fh.read(16)
                    if len(h) < 16:
                        break
                    rx, nb, _ = struct.unpack("<QII", h)
                    first = rx if first is None else first
                    last = rx
                    fh.seek(nb, 1)
                    n += 1
                    size += nb
            out["rds"][parts[1]] = {"chunks": n, "bytes": size,
                                    "seconds": round((last - first) / 1e9, 3) if n > 1 else 0}
    print(json.dumps(out, indent=2, default=str))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="theia tap", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record", help="record TIPC interfaces and RDS streams into <dir>")
    r.add_argument("dir")
    r.add_argument("--tipc", help="interfaces / messages / wire types, comma-separated")
    r.add_argument("--rds", help="RDS stream instances (/Writer/stream), comma-separated")
    r.add_argument("--seconds", type=float, default=0)
    r.add_argument("--join-timeout", type=float, default=120.0,
                   help="keep retrying pg joins this long (s) before reporting a group as not joined")
    p = sub.add_parser("replay", help="replay <dir> at its recorded times")
    p.add_argument("dir")
    p.add_argument("--speed", type=float, default=1.0)
    p.add_argument("--only", help="names (wire type / interface / message, or /stream) to replay")
    p.add_argument("--start-delay-ms", type=int, default=1000)
    for x in (r, p):
        x.add_argument("--instance", type=int, default=None,
                       help="the stack's supervisor instance (default: $THEIA_PG_SUPERVISOR_INSTANCE or 0)")
        x.add_argument("--ws", help="workspace whose system/ resolves the names (default $THEIA_WORKSPACE)")
    i = sub.add_parser("info", help="what <dir> holds")
    i.add_argument("dir")
    a = ap.parse_args(argv)
    if a.cmd != "info" and getattr(a, "speed", 1.0) <= 0:
        ap.error("--speed must be > 0")
    return {"record": cmd_record, "replay": cmd_replay, "info": cmd_info}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
