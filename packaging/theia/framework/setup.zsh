# /opt/theia/setup.zsh — activate Theia in this zsh (ROS2 setup.zsh analogue).
# Source it:  source /opt/theia/setup.zsh
# Same effect as setup.bash; zsh-clean (no bashisms).

: ${THEIA_ROOT:=/opt/theia}
export THEIA_ROOT

case ":${PATH}:" in
    *":${THEIA_ROOT}/bin:"*) ;;
    *) export PATH="${THEIA_ROOT}/bin:${PATH}" ;;
esac

for _pp in "${THEIA_ROOT}"/lib/python*/site-packages(N); do
    case ":${PYTHONPATH:-}:" in
        *":${_pp}:"*) ;;
        *) export PYTHONPATH="${_pp}${PYTHONPATH:+:${PYTHONPATH}}" ;;
    esac
done
unset _pp

echo "theia: activated (THEIA_ROOT=${THEIA_ROOT}); artheia $(artheia --version 2>/dev/null || echo '?')"
