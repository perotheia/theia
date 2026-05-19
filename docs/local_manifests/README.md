# Local manifests

Per-vendor or per-developer additions to the workspace that are **not**
part of `.repo/manifests/default.xml`. Each vendor implementation
(Tornado, etc.) lives in its own GitLab repo and is wired in by dropping
a `<vendor>.xml` file from this directory into `$WORKSPACE/.repo/local_manifests/`.

## How to use

```sh
cp docs/local_manifests/vendor_tornado.xml .repo/local_manifests/
repo sync vendor/tornado
```

After `repo sync`, the project is checked out and tracked like any other
manifest project (visible in `repo status`, kept up to date by `repo
sync`).

## Why local, not in the default manifest

The default manifest defines the canonical workspace that ships to
every PERO CMP developer. Vendor implementations are optional and may
have restricted access — they don't belong in the default. Local
manifests give each developer (or CI runner) control over which vendors
get pulled in.

`.repo/` itself is gitignored in pero_theia, so the local manifest does
not get committed. Distribute it via this directory (or a private
location) instead.

## Adding a new vendor

1. Push the vendor's `system/` tree to a fresh GitLab repo under
   `PG50/<vendor>.git` (or wherever your team agrees).
2. Copy `docs/local_manifests/vendor_tornado.xml` to a new file named
   `<vendor>.xml`; replace the project name + path accordingly.
3. Drop into `.repo/local_manifests/` on each workstation that needs
   the vendor.
