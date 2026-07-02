# colony S3-manifest patches (reference copies)

The AUTHORITATIVE source is the **colony repo** on dalek at
`/home/applied/repo/colony/ansible/` (volume-mounted into the `colony-api`
container at `/colony`). These are reference snapshots of the changes that make
colony read the deploy manifest FROM S3 instead of a local `$THEIA_WORKSPACE`.

Pair with the theia-side change: `theia release <services> --s3` /
`theia release-swp --s3` now upload the per-machine manifest+config to the S3
plane (`<key>/manifest.tar.gz` + `manifest/<machine>/*`).

Changes:
- **tasks/fetch-manifest-s3.yml** (NEW): on localhost in the resolve play, when
  `s3_endpoint` + `runtime_version` are set, download `<key>/manifest.tar.gz`
  over plain HTTP, unpack to a controller-local temp, repoint `manifest_dir` at
  it, and set `theia_run_src` to the cached `theia-run.sh`. NO-OP without S3.
- **orchestrate.yml / provision.yml**: include fetch-manifest-s3.yml before
  `add_host`; thread `manifest_dir` + `theia_run_src` through `add_host` so the
  play host reads the S3 cache.
- **tasks/install-supervisor-unit.yml**: `theia_run_src` empty-string fallback
  (use it when non-empty/S3, else the local relative path).

Verified: `colony orchestrate central` provisions PURELY from S3 with the local
`dist/manifest` removed. `deploy/registry/<target>.yml` (host resolution) still
roots at the local `$THEIA_WORKSPACE/deploy/registry` — that is NOT in S3.
