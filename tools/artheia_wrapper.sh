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
if [[ -n "$WORKSPACE" && -x "$WORKSPACE/.venv/bin/artheia" ]]; then
    export PATH="$WORKSPACE/.venv/bin:$PATH"
fi

exec artheia "$@"
