#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path


CANONICAL_TARGETS = ("x86_64", "arm64", "jetson")
REPO_COMMANDS = {
    "run",
    "build",
    "pipeline-app",
    "video-player",
    "dual-record",
    "dual-recordd",
    "dualctl",
    "gopro_remote_bridge",
}


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _dockerfile_path(repo_root: Path) -> Path:
    return repo_root / "env" / "deepstream" / "Dockerfile"


def _read_default_tag(repo_root: Path) -> str:
    tag_path = repo_root / "env" / "deepstream" / "tag"
    if tag_path.is_file():
        tag = tag_path.read_text(encoding="utf-8").strip()
        if tag:
            return tag
    return "hstream-deepstream"


def _run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(cmd))
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd is not None else None,
        env=env,
        check=check,
        text=True,
    )


def _docker_env() -> dict[str, str]:
    env = dict(os.environ)
    env.setdefault("DOCKER_BUILDKIT", "1")
    return env


def _docker_bridge_interface_exists() -> bool:
    return Path("/sys/class/net/docker0").exists()


def _default_build_network() -> str:
    return "default" if _docker_bridge_interface_exists() else "host"


def _default_run_network() -> str:
    return "bridge" if _docker_bridge_interface_exists() else "host"


def _has_nvidia_gpu() -> bool:
    if Path("/dev/nvidiactl").exists():
        return True
    try:
        subprocess.run(
            ["nvidia-smi", "-L"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        return True
    except Exception:
        return False


def _is_jetson_host() -> bool:
    if Path("/etc/nv_tegra_release").exists():
        return True
    compatible = Path("/proc/device-tree/compatible")
    if compatible.is_file():
        try:
            text = compatible.read_bytes().decode("utf-8", errors="ignore").lower()
            return "tegra" in text
        except Exception:
            return False
    return False


def _detect_host_target_platform() -> str:
    machine = os.uname().machine
    if machine == "x86_64":
        return "x86_64"
    if machine in ("aarch64", "arm64"):
        return "jetson" if _is_jetson_host() else "arm64"
    raise SystemExit(f"Unsupported host architecture: {machine}")


def _normalize_target_platform(value: str | None, *, default: str) -> str:
    if not value:
        return default
    normalized = value.strip().lower().replace("-", "_")
    aliases = {
        "amd64": "x86_64",
        "k8": "x86_64",
        "x64": "x86_64",
        "x86": "x86_64",
        "x86_64": "x86_64",
        "aarch64": "arm64",
        "arm64": "arm64",
        "sbsa": "arm64",
        "dgx_spark": "arm64",
        "spark": "arm64",
        "tegra": "jetson",
        "jetson": "jetson",
    }
    try:
        return aliases[normalized]
    except KeyError as exc:
        raise SystemExit(
            f"Unsupported target platform '{value}'. Expected one of: {', '.join(CANONICAL_TARGETS)}"
        ) from exc


def _docker_platform_for_target(target: str) -> str:
    return {
        "x86_64": "linux/amd64",
        "arm64": "linux/arm64",
        "jetson": "linux/arm64",
    }[target]


def _resolve_image_tag(repo_root: Path, tag_override: str | None, target: str) -> str:
    if tag_override:
        return tag_override
    return f"{_read_default_tag(repo_root)}-{target}"


def _docker_image_exists(tag: str) -> bool:
    try:
        subprocess.run(
            ["docker", "image", "inspect", tag],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
        return True
    except Exception:
        return False


def _resolve_run_image_tag(repo_root: Path, tag_override: str | None, target: str) -> str:
    resolved = _resolve_image_tag(repo_root, tag_override, target)
    if tag_override:
        return resolved

    if _docker_image_exists(resolved):
        return resolved

    fallback = _read_default_tag(repo_root)
    if _docker_image_exists(fallback):
        print(
            f"NOTE: Default tag '{resolved}' not found locally; using legacy tag '{fallback}'",
            file=sys.stderr,
        )
        return fallback

    return resolved


def _docker_image_user(tag: str) -> str | None:
    try:
        out = subprocess.check_output(
            ["docker", "image", "inspect", "--format", "{{.Config.User}}", tag],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        return out or None
    except Exception:
        return None


def _container_home_for_user(user: str | None, fallback_username: str) -> str:
    if not user or user in ("root", "0", "0:0"):
        return "/root"
    if ":" in user:
        return f"/home/{fallback_username}"
    return f"/home/{user}"


def _default_bazel_flags_for_target(target: str) -> list[str]:
    if target == "x86_64":
        return ["--config=opt", "--cpu=k8"]
    return ["--config=opt"]


def _has_explicit_bazel_platform_flags(args: list[str]) -> bool:
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ("--config", "--cpu", "--platforms", "--extra_toolchains"):
            return True
        if (
            arg.startswith("--config=")
            or arg.startswith("--cpu=")
            or arg.startswith("--platforms=")
            or arg.startswith("--extra_toolchains=")
        ):
            return True
        i += 1
    return False


def _bootstrap_bazelisk_snippet() -> str:
    return """
export PATH="$HOME/.local/bin:$PATH"
if ! command -v bazelisk >/dev/null 2>&1; then
  arch="$(uname -m)"
  case "${arch}" in
    x86_64) bazel_arch="amd64" ;;
    aarch64|arm64) bazel_arch="arm64" ;;
    *)
      echo "Unsupported architecture for bazelisk bootstrap: ${arch}" >&2
      exit 1
      ;;
  esac
  mkdir -p "$HOME/.local/bin"
  curl -fsSL -o "$HOME/.local/bin/bazelisk" \
    "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-${bazel_arch}"
  chmod +x "$HOME/.local/bin/bazelisk"
  ln -sf "$HOME/.local/bin/bazelisk" "$HOME/.local/bin/bazel"
fi
"""


def _shell_command(script: str) -> list[str]:
    body = "\n".join(line.rstrip() for line in script.strip().splitlines())
    return ["bash", "-lc", body]


def _shell_join(parts: list[str]) -> str:
    return shlex.join(parts)


def _resolve_repo_command(command: list[str], target: str, repo_mount: str | None) -> list[str]:
    if not command:
        return ["bash"]

    if command[0] == "--":
        command = command[1:]
        if not command:
            return ["bash"]

    cmd = command[0]
    rest = command[1:]

    if cmd in ("bash", "shell", "console"):
        return ["bash", *rest]

    if cmd not in REPO_COMMANDS:
        return command

    if not repo_mount:
        raise SystemExit(f"Command '{cmd}' requires the repo mount; omit --no-dev-mount.")

    default_bazel_flags = _default_bazel_flags_for_target(target)
    bazel_flags = _shell_join(default_bazel_flags)
    bootstrap = _bootstrap_bazelisk_snippet()

    if cmd == "build":
        build_args = rest or ["//..."]
        effective_args = build_args if _has_explicit_bazel_platform_flags(build_args) else default_bazel_flags + build_args
        return _shell_command(
            f"""
            set -euo pipefail
            {bootstrap}
            cd {shlex.quote(repo_mount)}
            exec bazelisk build {_shell_join(effective_args)}
            """
        )

    binary_targets = {
        "pipeline-app": (
            "//src/apps/pipeline-app:pipeline-app",
            "bazel-bin/src/apps/pipeline-app/pipeline-app",
        ),
        "video-player": (
            "//src/apps/video-player:video-player",
            "bazel-bin/src/apps/video-player/video-player",
        ),
        "dual-record": (
            "//src/apps/dual-record:dual-record",
            "bazel-bin/src/apps/dual-record/dual-record",
        ),
        "dual-recordd": (
            "//src/apps/dual-record:dual-recordd",
            "bazel-bin/src/apps/dual-record/dual-recordd",
        ),
        "dualctl": (
            "//src/apps/dual-record:dualctl",
            "bazel-bin/src/apps/dual-record/dualctl",
        ),
        "gopro_remote_bridge": (
            "//src/apps/dual-record:gopro_remote_bridge",
            "bazel-bin/src/apps/dual-record/gopro_remote_bridge",
        ),
    }

    if cmd == "run":
        return _shell_command(
            f"""
            set -euo pipefail
            {bootstrap}
            cd {shlex.quote(repo_mount)}
            bazelisk build {bazel_flags} //src/apps/pipeline-app:pipeline-app
            exec ./run.sh {_shell_join(rest)}
            """
        )

    target_label, binary_path = binary_targets[cmd]
    return _shell_command(
        f"""
        set -euo pipefail
        {bootstrap}
        cd {shlex.quote(repo_mount)}
        bazelisk build {bazel_flags} {shlex.quote(target_label)}
        exec ./{binary_path} {_shell_join(rest)}
        """
    )


def cmd_build(args: argparse.Namespace) -> None:
    repo_root = _repo_root()
    target = _normalize_target_platform(args.target_platform, default="x86_64")
    docker_platform = _docker_platform_for_target(target)
    tag = _resolve_image_tag(repo_root, args.tag, target)

    network = args.network
    if network == "bridge":
        network = "default"

    build_cmd = ["docker", "buildx", "build"]
    if args.builder:
        build_cmd += ["--builder", args.builder]

    build_cmd += [
        "--network",
        network,
        "--platform",
        docker_platform,
        "-f",
        str(_dockerfile_path(repo_root)),
        "-t",
        tag,
        "--build-arg",
        f"USERNAME={args.username}",
        "--build-arg",
        f"UID={args.uid}",
        "--build-arg",
        f"GID={args.gid}",
        "--build-arg",
        f"HSTREAM_DOCKER_TARGET_PLATFORM={target}",
    ]

    if args.push:
        build_cmd.append("--push")
    else:
        build_cmd.append("--load")

    build_cmd.append(str(repo_root))
    _run(build_cmd, env=_docker_env(), check=True)


def cmd_run(args: argparse.Namespace) -> None:
    repo_root = _repo_root()
    target = _normalize_target_platform(
        args.target_platform, default=_detect_host_target_platform()
    )
    tag = _resolve_run_image_tag(repo_root, args.tag, target)

    network = args.network
    if network == "default":
        network = "bridge"

    docker_cmd = ["docker", "run", "--rm", "-i"]
    if sys.stdin.isatty() and sys.stdout.isatty():
        docker_cmd.append("-t")
    docker_cmd += ["--network", network, "--shm-size", args.shm_size]

    if args.privileged:
        docker_cmd.append("--privileged")

    if args.name:
        docker_cmd += ["--name", args.name]

    docker_cmd += [
        "-e",
        f"HSTREAM_TARGET_PLATFORM={target}",
        "-e",
        "DEEPSTREAM_CONTAINER=1",
        "-e",
        "CUDA_CACHE_DISABLE=0",
    ]

    gpus: str | None
    if args.gpus == "auto":
        if _is_jetson_host():
            gpus = "jetson"
        elif _has_nvidia_gpu():
            gpus = "all"
        else:
            gpus = None
            print(
                "NOTE: No NVIDIA GPU/driver detected on the host; running without Docker GPU flags.",
                file=sys.stderr,
            )
    elif args.gpus in ("none", ""):
        gpus = None
    else:
        gpus = args.gpus

    if gpus == "jetson":
        docker_cmd += ["--runtime", "nvidia"]
        docker_cmd += ["-e", "NVIDIA_DRIVER_CAPABILITIES=all"]
    elif gpus is not None:
        docker_cmd += ["--gpus", gpus]
        docker_cmd += ["-e", "NVIDIA_DRIVER_CAPABILITIES=compute,utility,video"]

    display = os.environ.get("DISPLAY")
    if display and Path("/tmp/.X11-unix").exists():
        docker_cmd += ["-e", f"DISPLAY={display}"]
        docker_cmd += ["-e", "QT_X11_NO_MITSHM=1"]
        docker_cmd += ["-v", "/tmp/.X11-unix:/tmp/.X11-unix:rw"]

    if Path("/dev").exists():
        docker_cmd += ["-v", "/dev:/dev"]
    if Path("/dev/bus/usb").exists():
        docker_cmd += ["-v", "/dev/bus/usb:/dev/bus/usb"]
    if Path("/mnt").exists():
        docker_cmd += ["-v", "/mnt:/mnt"]

    pool_path = Path(f"/{os.environ.get('USER', 'user')}-pool")
    if pool_path.exists():
        docker_cmd += ["-v", f"{pool_path}:{pool_path}"]

    host_media = Path(f"/media/{os.environ.get('USER', '')}")
    if host_media.exists():
        docker_cmd += ["-v", f"{host_media}:{host_media}:slave"]

    image_user = _docker_image_user(tag)
    container_home = _container_home_for_user(image_user, args.username)

    if args.videos_mount:
        host_videos = Path(args.videos_mount).expanduser().resolve()
        host_videos.mkdir(parents=True, exist_ok=True)
        container_videos = f"{container_home}/Videos"
        print(
            f"NOTE: Mounting videos: {host_videos} -> {container_videos}"
            + (f" (image USER={image_user})" if image_user else ""),
            file=sys.stderr,
        )
        docker_cmd += ["-v", f"{host_videos}:{container_videos}:rw"]
        docker_cmd += ["-v", f"{host_videos}:/Videos:rw"]

    repo_mount = None
    container_workdir = None
    if args.dev_mount:
        repo_mount = "/workspace/hstream"
        docker_cmd += ["-v", f"{repo_root}:{repo_mount}:rw"]
        docker_cmd += ["-e", f"HSTREAM_REPO={repo_mount}"]
        container_workdir = repo_mount

    if args.workdir:
        host_workdir = Path(args.workdir).expanduser().resolve()
        host_workdir.mkdir(parents=True, exist_ok=True)
        docker_cmd += ["-v", f"{host_workdir}:/workspace/workdir:rw"]
        container_workdir = "/workspace/workdir"
        print(
            f"NOTE: Mounting workdir: {host_workdir} -> /workspace/workdir",
            file=sys.stderr,
        )

    if container_workdir:
        docker_cmd += ["-w", container_workdir]

    user_command = _resolve_repo_command(args.command, target, repo_mount)
    docker_cmd.append(tag)
    docker_cmd += user_command

    proc = _run(docker_cmd, check=False)
    if proc.returncode:
        raise SystemExit(proc.returncode)


def main(argv: list[str]) -> int:
    repo_root = _repo_root()
    default_tag = _read_default_tag(repo_root)
    default_build_network = _default_build_network()
    default_run_network = _default_run_network()

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--tag",
        default=None,
        help=(
            "Docker image tag. Defaults to env/deepstream/tag with a platform suffix "
            f"(base tag: {default_tag})."
        ),
    )
    common.add_argument(
        "--username",
        default=os.environ.get("USER", "hstream"),
        help="Username to bake into the image (default: current $USER)",
    )
    common.add_argument("--uid", type=int, default=os.getuid(), help="UID for image user")
    common.add_argument("--gid", type=int, default=os.getgid(), help="GID for image user")

    parser = argparse.ArgumentParser(
        prog="hstream_cuda_container.py",
        description="Build and run the hstream DeepStream Docker image.",
        parents=[common],
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    build = subparsers.add_parser("build", parents=[common], help="Build the Docker image")
    build.add_argument(
        "--target-platform",
        "--platform",
        dest="target_platform",
        default=None,
        help="Destination platform: x86_64, arm64, or jetson (default: x86_64)",
    )
    build.add_argument(
        "--network",
        default=default_build_network,
        help=f"Docker build network mode (default: {default_build_network})",
    )
    build.add_argument("--builder", default=None, help="Optional docker buildx builder name")
    build.add_argument(
        "--push",
        action="store_true",
        help="Push the image instead of loading it into the local Docker daemon",
    )
    build.set_defaults(func=cmd_build)

    run = subparsers.add_parser("run", parents=[common], help="Run a command in the image")
    run.add_argument(
        "--target-platform",
        "--platform",
        dest="target_platform",
        default=None,
        help="Destination platform: x86_64, arm64, or jetson (default: auto-detect host)",
    )
    run.add_argument(
        "--network",
        default=default_run_network,
        help=f"Docker run network mode (default: {default_run_network})",
    )
    run.add_argument("--name", default=None, help="Optional container name")
    run.add_argument(
        "--gpus",
        default="auto",
        help='Docker --gpus value (default: auto; use "none" to disable)',
    )
    run.add_argument(
        "--no-gpus",
        dest="gpus",
        action="store_const",
        const="none",
        help="Disable Docker GPU flags",
    )
    run.add_argument(
        "--videos-mount",
        default=str(Path.home() / "Videos"),
        help="Host videos directory to mount into the container (default: ~/Videos)",
    )
    run.add_argument(
        "--no-videos-mount",
        dest="videos_mount",
        action="store_const",
        const=None,
        help="Do not mount a host videos directory",
    )
    run.add_argument(
        "--dev-mount",
        dest="dev_mount",
        action="store_true",
        default=True,
        help="Bind-mount this repo into /workspace/hstream (default: enabled)",
    )
    run.add_argument(
        "--no-dev-mount",
        dest="dev_mount",
        action="store_false",
        help="Do not bind-mount this repo",
    )
    run.add_argument(
        "--workdir",
        default=None,
        help="Host directory to mount as /workspace/workdir and use as the container cwd",
    )
    run.add_argument("--shm-size", default="8g", help="Shared memory size (default: 8g)")
    run.add_argument(
        "--privileged",
        dest="privileged",
        action="store_true",
        default=True,
        help="Run the container in privileged mode (default: enabled)",
    )
    run.add_argument(
        "--no-privileged",
        dest="privileged",
        action="store_false",
        help="Do not run the container in privileged mode",
    )
    run.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        default=["bash"],
        help="Command to run in the container",
    )
    run.set_defaults(func=cmd_run)

    args = parser.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
