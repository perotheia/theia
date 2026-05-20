---
name: google-repo-tools
description: Expert guidance for Google Repo tool — the multi-repository Git management tool used in AOSP (Android Open Source Project) and other large codebases. Use this skill whenever the user mentions `repo`, `repo sync`, `repo init`, `repo upload`, `repo forall`, AOSP development, Android source, Gerrit uploads, manifest files, or multi-repo workflows. Trigger even for partial mentions like "repo command", "repo branch", "sync AOSP", ".repo directory", or "manifest branch". If the user is doing anything with the `repo` CLI tool, use this skill.
---

# Google Repo Tools

Guidance for using the `repo` CLI — the Git wrapper that coordinates work across the hundreds of Git repositories that make up AOSP and similar large projects.

## Core Concept

`repo` wraps Git. Each "project" is a separate Git repo mapped to a subdirectory. A **manifest** (XML) defines which repos exist and where they go. The `.repo/` directory at the root holds the manifest and repo metadata.

```
repo command [options] [project-list]
```

Project lists accept repo names or local paths.

---

## Essential Commands

### `repo init`
```bash
repo init -u <manifest-url> [-m <manifest.xml>] [-b <branch>]
```
- Creates `.repo/` in the current directory
- Common AOSP manifest: `https://android.googlesource.com/platform/manifest`
- `-b android-latest-release` recommended over `aosp-main` for stability
- Must be run before any other repo command

### `repo sync`
```bash
repo sync [project-list] [options]
```
Downloads and updates all (or specified) projects.

| Flag | Effect |
|------|--------|
| `-c` | Fetch only the current manifest branch |
| `-d` | Reset projects back to manifest revision |
| `-f` | Continue even if individual projects fail |
| `-j<N>` | Parallel threads (e.g. `-j8`); check `nproc --all` first |
| `-q` | Quiet mode |
| `-s` | Sync to known-good build per `manifest-server` |

**First sync** = equivalent to `git clone` for every project.  
**Subsequent sync** = `git remote update` + `git rebase origin/branch` per project.

Merge conflicts after sync → resolve with standard `git rebase --continue`.

### `repo start`
```bash
repo start <branch-name> [project-list]
```
Creates a new topic branch in the specified projects, starting from the manifest revision. Use `.` for the current project only.

**Always run `repo start` before making changes** — not doing so causes the "no branches ready for upload" error later.

### `repo upload`
```bash
repo upload [project-list] [--current-branch] [--topic=TOPIC] [-t]
```
Pushes local branches to Gerrit for code review.
- Requires HTTPS password configured at the Gerrit password generator
- Each commit becomes a separate Gerrit change
- `--replace PROJECT` — update existing changes (prompts for Gerrit change IDs)
- `-t` — set topic name same as the local branch name
- Squash commits first with `git rebase -i` if needed

> **Using GitLab instead of Gerrit?** See [GitLab Workarounds](#gitlab-workarounds) below.

### `repo status`
```bash
repo status [project-list]
```
Shows diff between HEAD, staging area, and working tree.

**Status codes** (two-letter, per file):

| Col 1 (staged vs HEAD) | Col 2 (workdir vs index) |
|------------------------|--------------------------|
| `A` Added | `m` Modified |
| `M` Modified | `d` Deleted |
| `D` Deleted | `-` New/unknown |
| `R` Renamed | |
| `C` Copied | |
| `T` Mode changed | |
| `U` Unmerged (conflict) | |

### `repo diff`
```bash
repo diff [project-list]
```
Shows uncommitted changes (`git diff` across all projects).

### `repo download`
```bash
repo download <target> <change-number>
```
Pulls a Gerrit change locally for testing.
```bash
repo download platform/build 23823
```
> Note: replication lag means new Gerrit changes may not be immediately downloadable.

> **Using GitLab instead of Gerrit?** See [GitLab Workarounds](#gitlab-workarounds) below.

### `repo forall`
```bash
repo forall [project-list] -c <shell-command>
```
Runs a shell command in every project. Available env vars:
- `REPO_PROJECT` — unique project name
- `REPO_PATH` — path relative to client root
- `REPO_REMOTE` — remote name from manifest
- `REPO_LREV` — manifest revision as local tracking branch
- `REPO_RREV` — manifest revision exactly as written

Flags: `-p` show project headers; `-v` show stderr output.

### `repo prune`
```bash
repo prune [project-list]
```
Deletes local topic branches that are already merged.

### `repo help`
```bash
repo help [command]
repo command --help
```

---

## Common Workflows

### Start a new feature
```bash
repo start my-feature frameworks/base  # or use . for current dir
# make changes...
repo status .
repo upload .
```

### Recover from "no branches ready for upload"
```bash
git commit -a                    # save local changes
repo start my-branch .           # create the branch
git reset --hard HEAD@{1}        # rewind to pre-repo-start state
repo upload .
```

### Run a command across all repos
```bash
repo forall -c 'git log --oneline -3'   # last 3 commits in every project
repo forall -p -c 'git status'          # status with project headers
```

### Clean up merged branches
```bash
repo prune
```

---

## Manifest Maintenance

### Manifest structure

A manifest is a Git repo containing an XML file (default `default.xml`). Repo fetches it on `repo init` and updates it on every `repo sync`. The three elements you touch most of the time:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>

  <!-- 1. Declare remotes (one per Git host) -->
  <remote name="aosp"
          fetch="https://android.googlesource.com"
          review="https://android-review.googlesource.com" />

  <remote name="vendor"
          fetch="https://gitlab.example.com/my-vendor"
          pushurl="git@gitlab.example.com:my-vendor" />

  <!-- 2. Set defaults so individual projects don't repeat themselves -->
  <default remote="aosp"
           revision="android-14.0.0_r1"
           sync-j="4" />

  <!-- 3. List projects -->
  <project name="platform/frameworks/base"
           path="frameworks/base" />

  <project name="my-hal"
           path="vendor/my/hal"
           remote="vendor"
           revision="main" />

</manifest>
```

### Rules for `<remote>`

- `fetch` is a **URL prefix**: repo appends `/${project_name}.git` automatically.  
  e.g. `fetch="https://android.googlesource.com"` + `name="platform/frameworks/base"` → clones `https://android.googlesource.com/platform/frameworks/base.git`
- `review` is needed only for `repo upload` to Gerrit; omit it for GitLab remotes.
- `pushurl` overrides the push URL (useful for SSH push vs HTTPS fetch).
- `alias` lets multiple remotes share the same local remote name in each project's `.git/config`.

### Rules for `<project>`

- `name` is the server-side path (appended to `fetch`); `path` is the local checkout location. If `path` is omitted, `name` is used.
- `revision` accepts branch names, tags, or SHA-1s. Overrides `<default revision>` for that project.
- `dest-branch` controls which Gerrit branch `repo upload` targets — set this explicitly when it differs from `revision`.
- `clone-depth="1"` for large binary projects to keep checkout shallow and fast.
- `groups="notdefault"` — project is **skipped** by `repo sync` unless explicitly requested. Use for optional or heavy vendor blobs.
- `sync-c="true"` — fetch only the tracked branch, not the full ref space. Speeds up sync on large projects.

### `<extend-project>` — patch without forking

Modify a project from the upstream manifest without replacing it. This is the preferred approach in local or vendor manifests because it survives upstream manifest updates gracefully:

```xml
<!-- Override the revision of an upstream project -->
<extend-project name="platform/frameworks/base"
                revision="refs/heads/my-vendor-patch" />

<!-- Redirect checkout path -->
<extend-project name="platform/hardware/interfaces"
                dest-path="vendor/acme/hardware/interfaces" />
```

Use `base-rev` to add a safety check — manifest parse fails if the upstream revision has changed since you set the override, preventing your patch from silently riding on a different commit.

### `<remove-project>` + replacement — vendor override pattern

The standard pattern for replacing an AOSP project with a vendor fork:

```xml
<!-- Remove the upstream project by name -->
<remove-project name="platform/hardware/libhardware" />

<!-- Re-add it from your remote, same path -->
<project name="libhardware-vendor"
         path="hardware/libhardware"
         remote="vendor"
         revision="vendor-customizations" />
```

- Use `optional="true"` if the project may not exist in all manifest variants.
- To remove by path only (name unknown): `<remove-project path="hardware/libhardware" />`.

### `<copyfile>` and `<linkfile>`

Run during `repo sync`. Paths are project-relative (src) and repo-root-relative (dest):

```xml
<project name="device/acme/common" path="device/acme/common">
  <copyfile src="sepolicy/file_contexts"
            dest="device/acme/sepolicy/file_contexts" />
  <linkfile src="Android.mk"
            dest="device/acme/Android.mk" />
</project>
```

- `copyfile` makes a physical copy; `linkfile` makes a symlink.
- Files are copied before links are created.
- Parent directories of `dest` are auto-created.

### `<include>` — split large manifests

```xml
<include name="vendor/acme/manifest.xml" />
```

Included manifest must be a valid standalone manifest. Groups from the `<include groups="...">` attribute cascade down into all included projects.

---

## Local Manifests

Local manifests let you add projects (vendor hardware, internal tools, private forks) to an existing repo checkout **without touching the upstream manifest**. This is the standard mechanism for vendor directories and per-developer customizations.

### Location and loading rules

```
$TOP_DIR/.repo/local_manifests/   ← directory (create if missing)
    local_manifest.xml            ← loaded alphabetically
    vendor_acme.xml               ←
    zzz_overrides.xml             ← loaded last (use z-prefix to control order)
```

- All `*.xml` files in `.repo/local_manifests/` are loaded **in alphabetical order** after the main manifest.
- The legacy single-file path `.repo/local_manifest.xml` (no directory, no `s`) is **not supported** — repo will ignore it silently.
- Projects defined here are auto-added to the `local::` group.

### Minimal local manifest

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>

  <remote name="vendor-private"
          fetch="git@gitlab.example.com:acme" />

  <project name="bsp/kernel-drivers"
           path="vendor/acme/kernel-drivers"
           remote="vendor-private"
           revision="main" />

</manifest>
```

After creating this file, `repo sync` will clone and keep the project in sync automatically.

### Vendor directory pattern

The typical full pattern when a vendor needs to: (a) add private projects, (b) replace an AOSP project with a fork, and (c) extend an upstream project's revision:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>

  <!-- Declare vendor remote -->
  <remote name="acme"
          fetch="https://gitlab.example.com/acme-android"
          pushurl="git@gitlab.example.com:acme-android" />

  <!-- Add new vendor-only projects -->
  <project name="vendor-hal"
           path="vendor/acme/hal"
           remote="acme"
           revision="android14-stable"
           clone-depth="1" />

  <project name="vendor-sepolicy"
           path="vendor/acme/sepolicy"
           remote="acme"
           revision="android14-stable" />

  <!-- Replace an AOSP project with vendor fork (same path) -->
  <remove-project name="platform/hardware/libhardware" />
  <project name="libhardware"
           path="hardware/libhardware"
           remote="acme"
           revision="android14-with-acme-patches" />

  <!-- Override revision of an AOSP project, keep everything else -->
  <extend-project name="platform/frameworks/native"
                  revision="refs/heads/acme/frameworks-native-patches" />

  <!-- Optional heavy blobs — only synced when explicitly requested -->
  <project name="vendor-blobs"
           path="vendor/acme/blobs"
           remote="acme"
           revision="main"
           groups="notdefault,vendor-blobs" />

</manifest>
```

To sync the optional blobs project explicitly:
```bash
repo sync --groups=default,vendor-blobs
```

### Local manifest gotchas

- **Alphabetical load order matters** — if you need a `<remote>` defined in `a_remotes.xml` to be available in `b_projects.xml`, the naming takes care of it; but if you need to override a project defined in the main manifest, your local manifest loads after, so `<remove-project>` + re-add always works.
- **`extend-project` vs `remove-project`+re-add** — prefer `extend-project` for simple attribute overrides (revision, groups); use remove+re-add only when you need a different remote or fundamentally different project definition.
- **Don't put local manifests in version control** (usually) — `.repo/` is typically gitignored. If you want a shareable vendor manifest, host it as a proper manifest repo and use `<include>` from the main manifest instead.
- **`local::` group** — all local manifest projects are in this group automatically. You can filter them: `repo sync --groups=default` skips all local manifest projects.

---

## GitLab Workarounds

`repo upload` and `repo download` are **Gerrit-native** — they speak Gerrit's proprietary ref protocol and will not work with GitLab out of the box. Here's why, and what to do instead.

### Why `repo upload` breaks on GitLab

`repo upload` pushes commits to Gerrit's magic refspec `refs/for/<branch>`, which Gerrit intercepts to create a code review. GitLab has no such hook — it only accepts pushes to `refs/heads/<branch>` (normal branches). Pushing to `refs/for/...` against a GitLab remote either fails outright or creates a strangely named branch, with no MR created.

**Workaround — push directly with `git` + open MR:**

```bash
# From inside the project subdirectory (e.g. cd frameworks/base)
git push origin HEAD:<your-branch-name>

# Then open the MR via GitLab UI, or use the GitLab CLI:
glab mr create --source-branch <your-branch-name> --target-branch <target>
```

For multi-project changes (the common repo scenario), script this with `repo forall`:

```bash
# Push every project's current branch to GitLab
repo forall -p -c \
  'git push origin HEAD:review/$REPO_PROJECT/my-feature 2>/dev/null || true'
```

This creates one branch per sub-project (`review/frameworks/base/my-feature`, etc.) which you then turn into MRs per project. There's no single "topic" abstraction across projects the way Gerrit has — that's the fundamental limitation.

**Squash before pushing** (same as with Gerrit):
```bash
git rebase -i HEAD~<n>   # squash checkpoint commits
git push origin HEAD:my-feature
```

---

### Why `repo download <target> <change-number>` breaks on GitLab

`repo download` fetches from Gerrit's internal ref namespace:
```
refs/changes/<last-two-digits-of-id>/<change-id>/<patchset>
```
e.g. `refs/changes/23/23823/1`. GitLab does not expose this namespace at all — it doesn't exist. Attempting `repo download` against a GitLab remote will fail with a fetch error because the ref cannot be found.

**Workaround — use a shared Git tag instead of a change number:**

The idea is to replace the opaque Gerrit change number with a human-readable, push-able, fetch-able Git tag that both sides agree on.

**Sender (the person sharing the change):**
```bash
# Tag the commit you want reviewed
git tag review/my-feature-v1          # or use SHA: git tag review/... <sha>
git push origin refs/tags/review/my-feature-v1
```

**Receiver (the person pulling it down to test):**
```bash
# From inside the relevant project directory
git fetch origin refs/tags/review/my-feature-v1
git checkout FETCH_HEAD              # detached HEAD for testing
# or create a local branch:
git checkout -b test/my-feature-v1 FETCH_HEAD
```

**Why tags work here:**
- Tags are immutable, named pointers — exactly what a Gerrit patchset number provides (patchset 1, patchset 2...).
- They're in the standard Git ref namespace, so any remote (GitLab, GitHub, Gitea) can host and serve them.
- Naming convention like `review/<feature>/<v1|v2|...>` gives you the same patchset iteration semantics Gerrit's change numbers provide.
- Unlike branches, tags don't move, so `v1` always means the exact commit that was reviewed, even after the branch is updated.

**Iterating on a review (equivalent to Gerrit patchset 2, 3...):**
```bash
# After amending or rebasing
git tag review/my-feature-v2
git push origin refs/tags/review/my-feature-v2
```

**Cleaning up after merge:**
```bash
git push origin :refs/tags/review/my-feature-v1   # delete remote tag
git push origin :refs/tags/review/my-feature-v2
git tag -d review/my-feature-v1 review/my-feature-v2  # delete local
```

---

### Summary: Gerrit vs GitLab equivalence

| Gerrit / repo native | GitLab workaround |
|----------------------|-------------------|
| `repo upload .` | `git push origin HEAD:<branch>` + open MR |
| `repo upload --topic=foo` | push per-project branches; group MRs by naming convention |
| `repo download platform/build 23823` | `git fetch origin refs/tags/review/<name>` + checkout |
| Gerrit patchset N | `git tag review/<feature>-vN` + push tag |
| Gerrit change ID | tag name (agreed naming convention replaces numeric ID) |

---

## Key Rules & Gotchas

- **No nested Git repos** — AOSP does not use `git submodule`. Each project is a flat sibling.
- **Working directory** — All `repo` commands (except `init`) must be run from inside the repo client root (parent of `.repo/`) or any subdirectory.
- **`repo sync` destroys `repo download` commits** — test Gerrit patches in a throwaway branch.
- **`-d` flag on sync** — useful when your project drifted to a topic branch but you need the manifest baseline.
- **Thread count** — `-j$(nproc)` is aggressive; leave headroom with `-j$(( $(nproc) / 2 ))` on busy machines.
- **2026 AOSP schedule** — source is published to AOSP in Q2 and Q4 only. Use `android-latest-release` branch, not `aosp-main`, for the most recent stable code.
