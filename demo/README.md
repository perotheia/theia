# demo — Theia consuming workspace

Built against a SIBLING Theia source checkout (THEIA_ROOT=/home/axadmin/repo/theia_github),
not vendored. system/platform/runtime + system/supervisor (+ system/services
with --with-services) are symlinks into it.

```sh
source /home/axadmin/repo/theia_github/env.sh       # activate the sibling framework checkout:
                                 # THEIA_ROOT + its .venv + `theia`/`tdb` on PATH,
                                 # THEIA_WORKSPACE=this dir
theia init                       # (already run — scaffolded this dir)
# link your app/gateway spec, import it in system/system.art, then:
theia manifest
theia install
theia start && tdb ps
```

When Theia ships as a deb, swap the sibling source checkout for the installed
prefix: `source /opt/theia/setup.sh` (THEIA_ROOT=/opt/theia) — the symlinks +
rig stay the same. (env.sh is the source-checkout activation; setup.sh ships
only inside the deb.)
