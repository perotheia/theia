"""A TEST-ONLY process + its own inbound ports (viz, bond005, step 8).

ProcessLayer.test_only / .ports serialize into execution.json; `theia install`
writes config/fw.d/<process>.nft for the ports (the fw FC merges it); production
builds (`theia dist`, `release-swp`) refuse a manifest that holds a test-only
process.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "artheia"))
import theia  # noqa: E402


def _manifest(root: Path, procs: list) -> Path:
    (root / "central").mkdir(parents=True)
    (root / "central" / "execution.json").write_text(json.dumps({"processes": procs}))
    return root


def test_install_writes_the_fw_fragment(tmp_path):
    m = _manifest(tmp_path / "manifest", [
        {"name": "viz", "test_only": True, "ports": [{"proto": "tcp", "port": 8090}]},
        {"name": "per", "ports": []},
    ])
    cfg = tmp_path / "install" / "config"
    cfg.mkdir(parents=True)
    theia._stage_fw_ports(m, "central", cfg)
    frag = (cfg / "fw.d" / "viz.nft").read_text()
    assert 'tcp dport 8090 accept comment "viz (test-only)"' in frag
    assert not (cfg / "fw.d" / "per.nft").exists()
    # a later install without the port removes OUR fragment, never a hand-written one
    (cfg / "fw.d" / "site.nft").write_text("tcp dport 22 accept\n")
    m2 = _manifest(tmp_path / "manifest2", [{"name": "viz"}])
    theia._stage_fw_ports(m2, "central", cfg)
    assert not (cfg / "fw.d" / "viz.nft").exists()
    assert (cfg / "fw.d" / "site.nft").exists()


def test_production_builds_refuse_test_only(tmp_path, capsys):
    m = _manifest(tmp_path / "m", [{"name": "viz", "test_only": True}, {"name": "per"}])
    assert theia._refuse_test_only("dist", m) is True
    assert "central/viz" in capsys.readouterr().err
    clean = _manifest(tmp_path / "c", [{"name": "per"}])
    assert theia._refuse_test_only("dist", clean) is False


def test_the_layer_fields_resolve_and_validate():
    from artheia.manifest.algebra import Explicit
    from artheia.manifest.deployment import (DeploymentLayer, ExecutionLayer, ProcessLayer,
                                             MachineSetLayer, MachineLayer)

    def dep(ports):
        return DeploymentLayer(
            execution=ExecutionLayer(processes={
                ProcessLayer(name="viz", executable=Explicit("//x:viz"), start_cmd=Explicit("bin/viz"),
                             function_group=Explicit("apps"), machine=Explicit("central"),
                             test_only=Explicit(True), ports=Explicit(ports))}),
            machines=MachineSetLayer(machines={MachineLayer(name="central")}))

    good = dep((("tcp", 8090),))
    assert not [i for i in good._invariants("rig") if "ports" in i.path]
    t = next(iter(good.simplify().execution.processes))
    assert t.test_only is True and t.ports == (("tcp", 8090),)
    bad = dep((("icmp", 8090), ("tcp", 70000)))
    assert len([i for i in bad._invariants("rig") if "ports" in i.path]) == 2
