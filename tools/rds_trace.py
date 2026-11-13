#!/usr/bin/env python3
"""Summarize an RDS latency trace (THEIA_RDS_TRACE=<dir> on every process of a stack).

Each process writes <dir>/rds-trace-<pid>.csv (services/rds/transport/iceoryx_transport.cc):
  pub,<stream>,<frame_id>,<Loan returned>,<Publish called>      (writer)
  take,<stream>,<frame_id>,<Take returned>,<Release called>     (reader)
CLOCK_REALTIME ns. A chunk is matched by (stream, frame_id).

Per chunk:  fill = loan -> publish (the writer's own work: encode, stamping, waits)
            rds  = publish -> take (iceoryx + the reader's poll wait)
            hold = take -> release (the reader's own work)
Per frame (the chunks of one frame_id; --streams to pick a set, e.g. a rig's 8):
            loan_spread = first -> last Loan (when the source handed the images over:
                          a writer loans as its callback starts)
            take_spread = first -> last Take
With --spr-timing <spr timing csv> (frame, capture_wall_ns, ...): capture -> first Loan.

    python3 tools/rds_trace.py <dir> [--streams /T/dep_fl,...] [--spr-timing lap-timing.csv]
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import sys
from collections import defaultdict


def pct(v: list[float], p: float) -> float | None:
    if not v:
        return None
    v = sorted(v)
    return round(v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))], 3)


def summarize(d: str, streams: list[str] | None = None, spr_timing: str | None = None) -> dict:
    pub: dict = {}
    take: dict = {}
    for f in glob.glob(f"{d}/rds-trace-*.csv"):
        for r in csv.DictReader(open(f)):
            if streams and r["stream"] not in streams:
                continue
            k = (r["stream"], int(r["frame_id"]))
            (pub if r["ev"] == "pub" else take)[k] = (int(r["t0_ns"]), int(r["t1_ns"]))
    ms = 1e6
    fill = [(b - a) / ms for a, b in pub.values()]
    hold = [(b - a) / ms for a, b in take.values()]
    rds = [(take[k][0] - pub[k][1]) / ms for k in pub if k in take]
    by_frame: dict[int, dict] = defaultdict(dict)
    for (s, fid), t in pub.items():
        by_frame[fid].setdefault("loan", []).append(t[0])
    for (s, fid), t in take.items():
        by_frame[fid].setdefault("take", []).append(t[0])
    want = len(streams) if streams else max((len(v.get("loan", [])) for v in by_frame.values()), default=0)
    full = {f: v for f, v in by_frame.items() if len(v.get("loan", [])) == want and len(v.get("take", [])) == want}
    loan_spread = [(max(v["loan"]) - min(v["loan"])) / ms for v in full.values()]
    take_spread = [(max(v["take"]) - min(v["take"])) / ms for v in full.values()]
    out = {"chunks": {"pub": len(pub), "take": len(take), "matched": len(rds)},
           "frames_complete": len(full), "streams_per_frame": want, "unit": "ms"}
    for name, v in (("fill", fill), ("rds", rds), ("hold", hold),
                    ("loan_spread", loan_spread), ("take_spread", take_spread)):
        out[name] = {"p50": pct(v, .5), "p99": pct(v, .99), "max": pct(v, 1.0)}
    if spr_timing:
        cap = {int(r["frame"]): int(r["capture_wall_ns"]) for r in csv.DictReader(open(spr_timing))}
        c2l = [(min(v["loan"]) - cap[f]) / ms for f, v in full.items() if f in cap]
        out["capture_to_first_loan"] = {"p50": pct(c2l, .5), "p99": pct(c2l, .99), "n": len(c2l)}
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--streams", help="comma-separated logical stream names (a frame = these)")
    ap.add_argument("--spr-timing", help="spr's timing csv (frame, capture_wall_ns, ...)")
    a = ap.parse_args(argv)
    out = summarize(a.dir, a.streams.split(",") if a.streams else None, a.spr_timing)
    print(json.dumps(out, indent=1))
    return 0 if out["chunks"]["matched"] else 1


if __name__ == "__main__":
    sys.exit(main())
