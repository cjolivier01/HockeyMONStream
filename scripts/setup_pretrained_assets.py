#!/usr/bin/env python3
"""Download pretrained assets declared by hstream YAML configs."""

from __future__ import annotations

import argparse
import hashlib
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path
from typing import Any, Iterable

try:
    import yaml
except ImportError as exc:
    raise SystemExit("PyYAML is required to read hstream config assets") from exc


ASSET_KEYS = ("pretrained-assets", "assets", "downloads")
YAML_SUFFIXES = {".yaml", ".yml"}


def _load_yaml(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _resolve_path(raw_path: str, base_dir: Path) -> Path:
    expanded = os.path.expandvars(os.path.expanduser(raw_path))
    path = Path(expanded)
    if not path.is_absolute():
        path = base_dir / path
    return path.resolve(strict=False)


def _iter_child_config_files(node: Any, base_dir: Path) -> Iterable[Path]:
    if isinstance(node, dict):
        if node.get("enable") is False or node.get("enable") == 0:
            return
        for key, value in node.items():
            if key == "config-file" and isinstance(value, str):
                child = _resolve_path(value, base_dir)
                if child.suffix in YAML_SUFFIXES:
                    yield child
            else:
                yield from _iter_child_config_files(value, base_dir)
    elif isinstance(node, list):
        for item in node:
            yield from _iter_child_config_files(item, base_dir)


def _normalize_asset_specs(value: Any) -> Iterable[dict[str, Any]]:
    if isinstance(value, list):
        for item in value:
            if isinstance(item, dict):
                yield item
        return

    if not isinstance(value, dict):
        return

    if "url" in value or "source" in value:
        yield value
        return

    for name, item in value.items():
        if isinstance(item, dict):
            spec = dict(item)
            spec.setdefault("name", name)
            yield from _normalize_asset_specs(spec)
        elif isinstance(item, list):
            yield from _normalize_asset_specs(item)


def _iter_asset_specs(config: Any) -> Iterable[dict[str, Any]]:
    if not isinstance(config, dict):
        return

    for key in ASSET_KEYS:
        if key in config:
            yield from _normalize_asset_specs(config[key])


def _asset_target_path(spec: dict[str, Any], config: Any, config_path: Path) -> Path:
    if "path" in spec:
        raw_path = str(spec["path"])
    elif "file" in spec:
        raw_path = str(spec["file"])
    elif "property" in spec:
        if not isinstance(config, dict) or not isinstance(config.get("property"), dict):
            raise ValueError(f"{config_path}: asset uses property={spec['property']!r}, but no property group exists")
        raw_path = str(config["property"].get(str(spec["property"]), ""))
        if not raw_path:
            raise ValueError(f"{config_path}: asset property {spec['property']!r} is not set")
    else:
        raise ValueError(f"{config_path}: asset needs 'path', 'file', or 'property'")

    return _resolve_path(raw_path, config_path.parent)


def _ensure_parent_dir(path: Path) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        return
    except OSError:
        pass

    real_parent = path.parent.resolve(strict=False)
    uid = os.getuid()
    gid = os.getgid()
    subprocess.run(["sudo", "mkdir", "-p", str(real_parent)], check=True)
    subprocess.run(["sudo", "chown", "-R", f"{uid}:{gid}", str(real_parent)], check=True)
    path.parent.mkdir(parents=True, exist_ok=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_sha256(path: Path, expected: str) -> None:
    actual = _sha256(path)
    if actual.lower() != expected.lower():
        raise ValueError(f"{path}: sha256 mismatch: expected {expected}, got {actual}")


def _download(url: str, path: Path, timeout: float) -> None:
    _ensure_parent_dir(path)
    request = urllib.request.Request(url, headers={"User-Agent": "hstream-asset-setup"})
    tmp_fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent))
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response, tmp_path.open("wb") as out:
            shutil.copyfileobj(response, out, length=1024 * 1024)
        tmp_path.replace(path)
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def _asset_command(spec: dict[str, Any]) -> list[str]:
    raw_command = spec.get("command") or spec.get("generate-command") or spec.get("generate_command")
    if not raw_command:
        return []
    if isinstance(raw_command, str):
        return shlex.split(raw_command)
    if isinstance(raw_command, list):
        return [str(item) for item in raw_command]
    raise ValueError("asset command must be a string or list")


def _generate_asset(command: list[str], target: Path, config_path: Path, args: argparse.Namespace) -> None:
    _ensure_parent_dir(target)
    repo_root = config_path.parent.parent
    substitutions = {
        "target": str(target),
        "config_dir": str(config_path.parent),
        "repo_root": str(repo_root),
    }
    formatted = [part.format(**substitutions) for part in command]
    if formatted and not Path(formatted[0]).is_absolute() and ("/" in formatted[0] or formatted[0].startswith(".")):
        formatted[0] = str((config_path.parent / formatted[0]).resolve(strict=False))
    if formatted and Path(formatted[0]).suffix == ".py":
        formatted.insert(0, sys.executable)

    print(f"Generating pretrained asset: {target}", flush=True)
    print("  " + " ".join(shlex.quote(part) for part in formatted), flush=True)
    if args.dry_run:
        return
    subprocess.run(formatted, cwd=str(repo_root), check=True)
    if not target.exists() or target.stat().st_size == 0:
        raise RuntimeError(f"{config_path}: command did not generate expected asset {target}")


def _patch_onnx_dynamic_batch(path: Path) -> bool:
    try:
        import numpy as np
        import onnx
        from onnx import numpy_helper
    except ImportError as exc:
        raise RuntimeError("onnx and numpy are required for onnx-dynamic-batch assets") from exc

    model = onnx.load(path)
    changed = False

    for value in list(model.graph.input) + list(model.graph.output):
        shape = value.type.tensor_type.shape
        if not shape.dim:
            continue
        dim = shape.dim[0]
        if dim.dim_param != "batch" or dim.dim_value:
            dim.ClearField("dim_value")
            dim.dim_param = "batch"
            changed = True

    for initializer in model.graph.initializer:
        arr = numpy_helper.to_array(initializer)
        if arr.dtype == np.int64 and arr.shape == (3,) and arr[0] == 1 and arr[-1] == -1:
            patched = arr.copy()
            patched[0] = 0
            initializer.CopyFrom(numpy_helper.from_array(patched, initializer.name))
            changed = True

    if changed:
        del model.graph.value_info[:]
        onnx.checker.check_model(model)
        onnx.save(model, path)

    return changed


def _process_asset(spec: dict[str, Any], config: Any, config_path: Path, args: argparse.Namespace) -> bool:
    url = str(spec.get("url") or spec.get("source") or "")
    command = _asset_command(spec)
    if not url and not command:
        raise ValueError(f"{config_path}: asset needs 'url', 'source', or 'command'")

    target = _asset_target_path(spec, config, config_path)
    downloaded = False
    if not target.exists() or target.stat().st_size == 0:
        if command:
            _generate_asset(command, target, config_path, args)
        else:
            print(f"Downloading pretrained asset: {target}")
            if args.dry_run:
                return True
            _download(url, target, args.timeout)
        downloaded = True

    expected_sha256 = spec.get("sha256")
    if expected_sha256 and not args.dry_run:
        _verify_sha256(target, str(expected_sha256))

    dynamic_batch = bool(spec.get("onnx-dynamic-batch") or spec.get("onnx_dynamic_batch"))
    if dynamic_batch and not args.dry_run:
        if _patch_onnx_dynamic_batch(target):
            print(f"Patched ONNX dynamic batch: {target}")
            downloaded = True

    return downloaded


def _collect_config_files(paths: Iterable[str]) -> list[Path]:
    pending = [Path(path).resolve() for path in paths]
    seen: set[Path] = set()
    ordered: list[Path] = []

    while pending:
        path = pending.pop(0)
        if path in seen or not path.exists() or path.suffix not in YAML_SUFFIXES:
            continue
        seen.add(path)
        ordered.append(path)
        config = _load_yaml(path)
        pending.extend(_iter_child_config_files(config, path.parent))

    return ordered


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("configs", nargs="+", help="YAML config files to scan")
    parser.add_argument("--dry-run", action="store_true", help="print downloads without writing files")
    parser.add_argument("--timeout", type=float, default=60.0, help="download timeout in seconds")
    args = parser.parse_args(argv)

    for config_path in _collect_config_files(args.configs):
        config = _load_yaml(config_path)
        for spec in _iter_asset_specs(config):
            _process_asset(spec, config, config_path, args)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
