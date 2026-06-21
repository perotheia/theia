# Theia environment (zsh) — source this to put a Theia checkout on your PATH,
# the ROS2 `setup.zsh` analogue for a DEV checkout (not an installed /opt/theia,
# which ships its own setup.zsh).
#
#   source /path/to/theia/setup.zsh
#
# This is a thin zsh entrypoint over setup.sh (which is already zsh-clean):
# keeping one body avoids the two drifting. setup.sh resolves THEIA_ROOT from
# its own location (handling %N under zsh), prepends .venv/bin + the `theia`
# launcher to PATH, and puts artheia on PYTHONPATH. A CONSUMING workspace
# (e.g. demo/) sources the SIBLING theia's setup.zsh from its own root, so $PWD
# at source time becomes THEIA_WORKSPACE.

# Resolve this file's dir under zsh and source the shared body next to it.
source "${0:A:h}/setup.sh"
