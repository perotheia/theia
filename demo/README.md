# demo — Theia consuming workspace

Built against a SIBLING Theia source checkout (THEIA_ROOT=/home/axadmin/repo/theia_github),
not vendored. system/platform/runtime + system/supervisor (+ system/services
with --with-services) are symlinks into it.

```sh
source /home/axadmin/repo/theia_github/setup.sh     # exports THEIA_ROOT, puts `theia` on PATH
theia init                       # (already run — scaffolded this dir)
# link your app/gateway spec, import it in system/system.art, then:
theia manifest
theia install
theia start && tdb ps
```

When Theia ships as a deb, swap the sibling source for /opt/theia
(THEIA_ROOT=/opt/theia) — the symlinks + rig stay the same.
