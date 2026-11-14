"""theia install / manifest argument + label handling (bond003's findings, 2026-11-13):
an external executable label stages from bazel-bin/external/<repo>+/ (not bazel-bin/@repo/),
@pero_theia from the framework's build; a flag's VALUE is never the target / machine."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "artheia"))
import theia  # noqa: E402


def test_external_labels():
    ws = theia.WORKSPACE if (theia.WORKSPACE / "MODULE.bazel").is_file() else theia.THEIA_ROOT
    assert theia._bazel_bin("@carlaviz//backend:carla_gt") == ws / "bazel-bin/external/carlaviz+/backend/carla_gt"
    assert theia._bazel_bin("@@carlaviz+//backend:carla_gt") == ws / "bazel-bin/external/carlaviz+/backend/carla_gt"
    assert theia._bazel_bin("@pero_theia//services/com/main:com") == theia.THEIA_ROOT / "bazel-bin/services/com/main/com"
    assert theia._bazel_bin("//services/com/main:com") == theia.THEIA_ROOT / "bazel-bin/services/com/main/com"


def test_attr_value_is_not_the_machine(monkeypatch, tmp_path):
    seen = {}

    def fake(*a, **k):                 # stop right after the argument parse
        raise SystemExit(0)
    monkeypatch.setattr(theia, "_check_tipc_addresses", lambda: (seen.setdefault("ok", 1), 1)[1])
    rc = theia.cmd_install(["carla", "--attr", "RIG_VIZ"])
    # with --attr's value no longer a positional, the parse proceeds to the address gate
    assert seen.get("ok") == 1 and rc == 1
