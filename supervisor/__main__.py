"""Supervisor CLI entry point.

Usage:
    python -m supervisor run manifest.yaml [--log-level INFO]

The supervisor process reads the manifest, builds an internal tree, and
runs the children. It is *not* daemonised — the caller (init system,
docker, the developer's terminal) is responsible for backgrounding it.
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
from pathlib import Path

import yaml

from supervisor.runtime import Supervisor


def main() -> int:
    ap = argparse.ArgumentParser(prog="supervisor")
    sub = ap.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="Run the supervisor tree.")
    run.add_argument("manifest", help="Path to executor.yaml")
    run.add_argument("--log-level", default="INFO",
                     choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    run.add_argument(
        "--root-dir",
        default=".",
        help="Working directory for child processes (cmd lookup root).",
    )

    args = ap.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if args.cmd == "run":
        manifest = yaml.safe_load(Path(args.manifest).read_text())
        sup = Supervisor(manifest, root_dir=Path(args.root_dir).resolve())
        # Forward signals to the supervisor for graceful shutdown.
        signal.signal(signal.SIGINT, sup.request_shutdown)
        signal.signal(signal.SIGTERM, sup.request_shutdown)
        return sup.run()

    ap.error(f"unknown command: {args.cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
