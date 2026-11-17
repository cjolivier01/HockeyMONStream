"""Operator-owned resource profiles and frozen runtime preflight."""

import copy
import os
from pathlib import Path

from .files import file_digest, private_directory, read_json
from .schema import digest, identifier, positive, require, validate_profile

DEFAULT_FIELDS = {
    "max_parallel",
    "poll_interval_ms",
    "command_timeout_seconds",
    "shutdown_timeout_seconds",
    "observer_timeout_seconds",
}


def controller_files():
    package = Path(__file__).resolve().parent
    return [package.parent / "hstream-workflow", *sorted(package.glob("*.py"))]


def deployed_configuration(config, registry):
    """Upgrade setup-created runtime copies to their recorded installation paths.

    Resource choices and admitted computers are unchanged. A draft never adds
    computers, capabilities or allocations to the installed configuration.
    """
    setup_path = Path(registry) / "setup.json"
    if not setup_path.is_file():
        return config
    draft = read_json(setup_path)
    saved = {"revision": digest(draft)}
    if not isinstance(draft, dict):
        return config
    result = copy.deepcopy(config)
    for key, profile in result["profiles"].items():
        runtime = profile["runtime"]
        old = Path(runtime.get("root", "/"))
        if "setup-runtimes" not in old.parts:
            continue
        if key == config["admission_profile"]:
            source = draft.get("controller_runtime")
        else:
            candidates = [
                name
                for name in draft.get("profiles", {})
                if key == name or key.startswith(name + "-")
            ]
            source = (
                draft["profiles"][max(candidates, key=len)]["runtime_root"]
                if candidates
                else None
            )
        require(
            isinstance(source, str)
            and Path(source).is_absolute()
            and "setup-runtimes" not in Path(source).parts,
            "The saved setup has no deployment folder. Recover setup and choose the installed HStream folder.",
        )
        active_path = Path(registry) / "setup-active.json"
        require(
            active_path.is_file(),
            "Apply the setup once to switch from copied programs to the deployment folder",
        )
        active = read_json(active_path)
        require(
            active.get("revision") == saved["revision"]
            and active.get("configuration_revision") == digest(config),
            "Setup has unfinished edits. Apply those choices before switching to the deployment folder",
        )
        root = Path(source)

        def remap(value, root=root, old=old):
            return str(root / Path(value).relative_to(old))

        profile["task_command"][0] = remap(profile["task_command"][0])
        for name in ("worker", "cli", "exporter", "config"):
            if name in runtime:
                runtime[name] = remap(runtime[name])
        runtime.update(root=str(root), installation="deployed", files={})
        if profile["executor"] == "slurm":
            runtime["architecture"] = "native"
            for evidence in profile["qualified_nodes"].values():
                evidence["runtime_identity"] = copy.deepcopy(runtime)
    return result


def load_config(registry):
    root = private_directory(registry)
    config = read_json(root / "config.json")
    required = {
        "schema",
        "defaults",
        "profiles",
        "allowed_game_roots",
        "observer_registry",
        "admission_profile",
    }
    require(
        set(config) == required and config["schema"] == 1,
        "Invalid workflow registry configuration",
    )
    config = deployed_configuration(config, root)
    defaults = config["defaults"]
    require(
        isinstance(defaults, dict) and set(defaults) == DEFAULT_FIELDS,
        "Workflow defaults must be explicitly resolved from the baseline",
    )
    for key, value in defaults.items():
        positive(value, key)
    require(
        isinstance(config["profiles"], dict) and config["profiles"],
        "Install at least one qualified resource profile",
    )
    pools = {}
    for key, profile in config["profiles"].items():
        identifier(key, "profile ID")
        validate_profile(profile)
        # Aliases cannot grant larger independent counters for the same pool.
        signature = (
            profile["limits"],
            profile["executor"],
            profile["resources"]["devices"],
        )
        require(
            profile["pool"] not in pools or pools[profile["pool"]] == signature,
            "Profiles sharing a pool must share limits and device inventory",
        )
        pools[profile["pool"]] = signature
    admission = config["profiles"].get(config["admission_profile"])
    require(
        admission is not None
        and admission["executor"] == "local"
        and admission["resources"]["gpus"] == 0,
        "Admission requires an explicitly configured local CPU profile",
    )
    roots = config["allowed_game_roots"]
    require(
        isinstance(roots, list) and roots, "Explicit allowed game roots are required"
    )
    for path in roots:
        require(
            isinstance(path, str) and Path(path).is_absolute(),
            "Invalid allowed game root",
        )
    observer = config["observer_registry"]
    require(
        observer is None or isinstance(observer, str) and Path(observer).is_absolute(),
        "Invalid observer registry",
    )
    return config


def validate_game(config, game):
    game = Path(game).resolve(strict=True)
    require(game.is_dir(), "Game directory is unavailable")
    require(
        any(
            game.is_relative_to(Path(root).resolve(strict=True))
            for root in config["allowed_game_roots"]
        ),
        "Game is outside approved workflow roots",
    )
    return game


def preflight_runtime(profile, verified=None):
    validate_profile(profile)
    if profile["runtime"].get("installation") == "deployed":
        require(
            os.access(profile["task_command"][0], os.X_OK),
            "HStream is not installed in the configured deployment folder",
        )
        return
    for path, expected in profile["runtime"]["files"].items():
        if verified is not None and (path, expected) in verified:
            continue
        # Runtime binaries can be large, but must still be bounded. This is a
        # protocol safety bound, not an application configuration default.
        actual = file_digest(path, 8 * 1024**3)
        require(
            actual["sha256"] == expected,
            "The frozen native deployment changed: " + path,
        )
        if verified is not None:
            verified.add((path, expected))
    require(
        os.access(profile["task_command"][0], os.X_OK),
        "Native task executable is not executable",
    )
