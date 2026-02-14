#!/usr/bin/env python3
"""
Install host-side dependencies needed for Jetson/aarch64 cross-builds:
 - docker (for buildx)
 - docker-buildx-plugin (if not bundled with docker)
 - qemu-user-static + binfmt-support (for running arm64 binaries via binfmt)
"""

import os
import shutil
import subprocess
import sys
from typing import List


def run(cmd: List[str]) -> None:
  print(f"+ {' '.join(cmd)}")
  subprocess.run(cmd, check=True)


def apt_install(packages: List[str]) -> None:
  run(["sudo", "apt-get", "update"])
  run(["sudo", "apt-get", "install", "-y"] + packages)


def main() -> int:
  if os.geteuid() != 0 and not shutil.which("sudo"):
    print("This script needs sudo to install packages.", file=sys.stderr)
    return 1

  pkgs = [
      "docker.io",
      "docker-buildx-plugin",
      "qemu-user-static",
      "binfmt-support",
  ]
  apt_install(pkgs)

  print("\nHost dependencies installed. You may need to restart the docker service or relogin for group permissions.")
  print("Verify buildx: `docker buildx version`")
  print("Verify qemu binfmt: `update-binfmts --display | grep qemu-aarch64`")
  return 0


if __name__ == "__main__":
  sys.exit(main())
