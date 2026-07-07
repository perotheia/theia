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
- **tasks/install-verify-key.yml** (NEW): pull the SWP artifact-verify PUBLIC key
  from `<bucket>/provisioning/artifact-verify-key.pem` onto the rig at
  `/etc/mender/artifact-verify-key.pem` and ensure mender.conf's
  `ArtifactVerifyKey` points at it — so the device REFUSES any SWP not signed by
  the operator's key. NO-OP without `s3_endpoint`. Include from
  `tasks/install-mender.yml` after mender.conf is written. The key is uploaded by
  `theia cert copy` and NEVER baked into the image or committed (`theia cert
  generate` regenerates the pair per deployment). Pair with the theia-side verbs:
  `theia cert generate` → `theia cert copy --s3 <url>` → `theia release-swp`.

Verified: `colony orchestrate central` provisions PURELY from S3 with the local
`dist/manifest` removed. `deploy/registry/<target>.yml` (host resolution) still
roots at the local `$THEIA_WORKSPACE/deploy/registry` — that is NOT in S3.
The verify-key flow is validated end-to-end: a SWP signed with the generated
private key validates against the public key fetched back from S3, and
mender-update REFUSES an unsigned/tampered artifact on-device (docker rig).
