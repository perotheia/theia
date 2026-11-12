"""`theia init --kind package` default TIPC address — per package, not shared.

Every package scaffold used to get its node at 0x800100F0, so two packages on
one host (or composed into one workspace) collided, and the address sat inside
the framework services' 0x8001xxxx range. The default is now derived from the
package name (0xD0HHHH01); the scaffold's probe reads the type back from the
package's own .art instead of a hardcoded constant.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import theia  # noqa: E402  (tools/theia.py)


def test_default_is_in_the_app_range_with_node_index_01():
    for name in ("probe", "spr", "sensor", "filter", "v2v", "a", "my-pkg"):
        t = int(theia._default_pkg_tipc(name), 16)
        assert t >> 24 == 0xD0, hex(t)
        assert t & 0xFF == 0x01, hex(t)


def test_deterministic():
    assert theia._default_pkg_tipc("spr") == theia._default_pkg_tipc("spr")


def test_distinct_packages_get_distinct_addresses():
    names = ["probe", "spr", "sensor", "filter", "v2v", "location", "chairio",
             "percept", "metis", "pa", "pb"]
    addrs = {theia._default_pkg_tipc(n) for n in names}
    assert len(addrs) == len(names)


def test_never_the_workspace_scaffold_default_or_framework_range():
    for i in range(5000):
        t = int(theia._default_pkg_tipc(f"pkg{i}"), 16)
        assert t != 0xD0010001                 # the `theia init` ws node
        assert (t >> 16) != 0x8001             # the framework services


def test_package_templates_carry_the_placeholder_not_a_constant():
    assert "0x800100F0" not in theia._PKG_PACKAGE_ART
    assert "@TIPC@" in theia._PKG_PACKAGE_ART
    assert "0x800100F0" not in theia._PKG_TEST_LIB
    assert "_node_tipc_type()" in theia._PKG_TEST_LIB


# ---- `theia start` arg parsing: an option VALUE is not the machine name -------

def test_install_dir_skips_the_instance_value(tmp_path, monkeypatch):
    monkeypatch.setattr(theia, "WORKSPACE", tmp_path)
    monkeypatch.delenv("THEIA_MACHINE", raising=False)
    assert theia._install_dir(["-d", "--instance", "2"]).name == "central"
    assert theia._install_dir(["--instance", "2", "-d"]).name == "central"
    assert theia._install_dir(["compute", "--instance", "3"]).name == "compute"
    assert theia._install_dir(["--instance", "3", "compute"]).name == "compute"
