# /opt/theia/setup.bash — activate Theia in this shell (ROS2 /opt/ros/<distro>/
# setup.bash analogue). Source it:  source /opt/theia/setup.bash
#
# Prepends the Theia install to PATH (artheia/theia/tdb CLIs + staged binaries)
# and PYTHONPATH (the artheia package ships under /opt/theia/lib, NOT system
# site-packages — only its third-party deps go to the system via the deb's
# postinst). THEIA_ROOT lets a workspace's bazel rules / setup.sh find the
# installed runtime sources.

_THEIA_ROOT="${THEIA_ROOT:-/opt/theia}"
export THEIA_ROOT="$_THEIA_ROOT"

# artheia + its console entry points.
case ":${PATH}:" in
    *":${_THEIA_ROOT}/bin:"*) ;;
    *) export PATH="${_THEIA_ROOT}/bin:${PATH}" ;;
esac

# The Theia Python lives under /opt/theia/lib/python*/site-packages.
for _pp in "${_THEIA_ROOT}"/lib/python*/site-packages; do
    [ -d "$_pp" ] || continue
    case ":${PYTHONPATH:-}:" in
        *":${_pp}:"*) ;;
        *) export PYTHONPATH="${_pp}${PYTHONPATH:+:${PYTHONPATH}}" ;;
    esac
done
unset _pp _THEIA_ROOT

echo "theia: activated (THEIA_ROOT=${THEIA_ROOT}); artheia $(artheia --version 2>/dev/null || echo '?')"
