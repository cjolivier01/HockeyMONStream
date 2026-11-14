"""Route registry clients to the operator's pinned controller deployment."""

import os
from pathlib import Path

from .files import file_digest
from .profiles import load_config
from .schema import require


def installed_launcher(config):
    profiles = config["profiles"]
    pins = profiles[config["admission_profile"]]["runtime"]["files"]
    candidates = [
        Path(path)
        for path in pins
        if Path(path).name == "hstream-workflow"
        and str(Path(path).parent / "hstream_workflow" / "cli.py") in pins
    ]
    require(
        len(candidates) == 1,
        "Workflow service configuration must pin exactly one installed controller "
        "in its admission profile; repair the registry configuration",
    )
    launcher = candidates[0]
    package = launcher.parent / "hstream_workflow"
    # Inspect the selected deployment, not this client's module inventory. A
    # newer checkout must remain able to use an older installed controller.
    modules = set(package.glob("*.py"))
    declared = {
        Path(path)
        for path in pins
        if Path(path).parent == package and Path(path).suffix == ".py"
    }
    require(
        modules == declared
        and {package / "__init__.py", package / "cli.py"} <= modules,
        "The installed workflow controller package is incomplete or changed; "
        "restore its pinned deployment before starting a workflow",
    )
    for path in [launcher, *sorted(modules)]:
        expected = pins[str(path)]
        require(
            all(
                profile["runtime"]["files"].get(str(path), expected) == expected
                for profile in profiles.values()
            ),
            "Workflow profiles refer to different controller deployments; "
            "repair the registry configuration",
        )
        require(
            file_digest(path, 8 * 1024**3)["sha256"] == expected,
            "The installed workflow controller changed: " + str(path),
        )
    require(
        os.access(launcher, os.X_OK),
        "The installed workflow controller is not executable: " + str(launcher),
    )
    return launcher


def dispatch(registry, argv):
    launcher = installed_launcher(load_config(registry))
    current = Path(__file__).resolve().parents[1] / "hstream-workflow"
    if launcher != current:
        # Keep the same PID, arguments, streams and exit status: the desktop
        # must still be able to stop a command when its deadline expires.
        # Client Python search paths (including Bazel's safe-path environment)
        # must not shadow the selected package and route back into this client.
        environment = dict(os.environ)
        for key in ("PYTHONPATH", "PYTHONHOME", "PYTHONSAFEPATH"):
            environment.pop(key, None)
        os.execve(str(launcher), [str(launcher), *argv], environment)
