"""theia start --instance N: the config-defaults seed targets THAT stack's per.

An isolated stack (theia start --instance N) runs its own per at TIPC instance N
(the supervisor shifts every child). The seed used to call per at its declared
instance 0, so on :N it timed out (or seeded another stack's per). PerClient now
carries the target instance; seed.py takes --instance; theia start passes it for
an isolated stack only (a rig's machine N keeps the master's per at :0).
"""
import dataclasses
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tdb"))
sys.path.insert(0, str(Path(__file__).resolve().parent))


@dataclasses.dataclass
class _Ref:
    name: str
    tipc_type: int = 0x80010007
    tipc_instance: int = 0


class _Ctx:
    def ref(self, node):
        return _Ref(node)

    def probe(self, *_a, **_k):
        class _P:
            def start(self):
                return self
        return _P()


def test_default_targets_the_declared_instance():
    from tdb_client import PerClient
    c = PerClient(_Ctx(), "/x")
    assert c.target("PerClient") == "PerClient"


def test_instance_overrides_the_target():
    from tdb_client import PerClient
    c = PerClient(_Ctx(), "/x", instance=3)
    t = c.target("PerClient")
    assert t.name == "PerClient" and t.tipc_instance == 3


def test_seed_cli_accepts_instance_and_theia_passes_it():
    src = (Path(__file__).resolve().parent / "migrate" / "seed.py").read_text()
    assert '"--instance"' in src and 'per.target("PerClient")' in src
    import theia
    import inspect
    body = inspect.getsource(theia._seed_defaults)
    assert '"--instance"' in body
    assert "_seed_defaults(int(instance) if _isolated else None)" in inspect.getsource(theia.cmd_start)
