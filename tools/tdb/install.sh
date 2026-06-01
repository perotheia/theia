#!/usr/bin/env bash
# Install `tdb` on PATH as a symlink into the workspace .venv/bin, mirroring
# how `.venv/bin/theia` → theia.py is wired. Idempotent.
#
#   bash tools/tdb/install.sh
#
# Needs prompt_toolkit (the REPL) — installed into the same venv:
#   pip install -r tools/tdb/requirements.txt
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ln -sf "$REPO/tools/tdb/tdb.py" "$REPO/.venv/bin/tdb"
chmod +x "$REPO/tools/tdb/tdb.py"
echo "installed: .venv/bin/tdb -> tools/tdb/tdb.py"
echo "try: tdb help   |   tdb ps   |   tdb   (REPL)"
