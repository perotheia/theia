# /opt/theia/setup.sh — activate Theia in this shell (ROS2 /opt/ros/<distro>/
# setup.sh analogue). Sourced the same way from bash, zsh, or dash:
#   source /opt/theia/setup.sh
#
# This is the CANONICAL, single activation entry — the one the tutorial, the
# generated setup_local.sh, and the workspace bazel rules all reference. One
# POSIX file serves every shell.
#
# It exports just two things:
#   PATH        prepend /opt/theia/bin (the `theia`/`artheia`/`tdb` shims +
#               the staged service/supervisor binaries).
#   THEIA_ROOT  the framework prefix everything resolves against.
#
# It deliberately DOES NOT touch PYTHONPATH: artheia lives in the user's own
# venv (installed from /opt/theia/wheels), and the framework's services manifest
# is loaded BY PATH from $THEIA_ROOT/manifest/ by the generated rig — so nothing
# Theia ships needs a generic package on the user's global import namespace.

_THEIA_ROOT="${THEIA_ROOT:-/opt/theia}"
export THEIA_ROOT="$_THEIA_ROOT"

# artheia + its console entry points.
case ":${PATH}:" in
    *":${_THEIA_ROOT}/bin:"*) ;;
    *) export PATH="${_THEIA_ROOT}/bin:${PATH}" ;;
esac

unset _THEIA_ROOT

# Report activation. artheia lives in the user's venv (installed from
# /opt/theia/wheels), which may not exist yet at source time — so only show its
# version if it actually resolves, rather than printing a bare "?".
if _av="$(artheia --version 2>/dev/null)"; then
    echo "theia: activated (THEIA_ROOT=${THEIA_ROOT}); ${_av}"
else
    echo "theia: activated (THEIA_ROOT=${THEIA_ROOT}); artheia not in venv yet" \
         "— pip install --find-links ${THEIA_ROOT}/wheels artheia"
fi
unset _av
