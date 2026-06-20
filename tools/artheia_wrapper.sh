#!/usr/bin/env bash
# tools/artheia_wrapper.sh — Bazel-callable artheia driver.
#
# Bazel actions don't inherit the user's PATH by default. This wrapper
# prepends the workspace's .venv/bin so `artheia` resolves, then execs
# it with the passed args. Used by the synthetic @rig_<name>// repo's
# genrules emitted by //rules:rig.bzl.
#
# .bazelrc already has `build --action_env=PATH` which propagates the
# user's PATH into actions — so usually the user's .venv is found.
# This wrapper handles the case where the user invoked bazel without
# the venv on PATH (e.g. from a CI runner with a system Python).

set -euo pipefail

# BUILD_WORKSPACE_DIRECTORY is set by `bazel run`; for `bazel build`
# actions, BUILD_WORKING_DIRECTORY is set instead. Fall back to
# walking up from $PWD to find the MODULE.bazel.
find_workspace() {
    local d
    d="${BUILD_WORKSPACE_DIRECTORY:-${BUILD_WORKING_DIRECTORY:-$PWD}}"
    while [[ "$d" != "/" ]]; do
        if [[ -f "$d/MODULE.bazel" ]]; then
            echo "$d"
            return 0
        fi
        d="$(dirname "$d")"
    done
    return 1
}

WORKSPACE="$(find_workspace || true)"
if [[ -n "$WORKSPACE" ]]; then
    if [[ -x "$WORKSPACE/.venv/bin/artheia" ]]; then
        export PATH="$WORKSPACE/.venv/bin:$PATH"
    fi
    # PYTHONPATH = workspace root so `artheia executor emit manifest.rig` can
    # import the workspace's OWN rig + generated manifest modules (the rig.py
    # path is relative to the consuming workspace, not the framework). The rig
    # genrules run `local` (un-sandboxed) so $WORKSPACE here is the real
    # execroot/_main, where the workspace's manifest/ package is symlinked in.
    export PYTHONPATH="${WORKSPACE}${PYTHONPATH:+:$PYTHONPATH}"
fi

exec artheia "$@"
