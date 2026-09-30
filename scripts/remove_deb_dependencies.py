#!/usr/bin/env python3
"""Relax dependency constraints in a Debian package.

Two kinds of pin keep an Ubuntu 24.04 artifact from installing on 26.04, and
both are relaxed by default.

Relationship versions carrying Ubuntu 24.04-specific package tags are dropped:

  libfoo (= 23.0.4-0ubuntu1~24.04.1) -> libfoo

Dependencies naming a specific CUDA minor toolkit are rewritten onto the
minor-independent virtual package that every CUDA 13 component provides:

  cuda-cudart-13-0 | cuda-cudart-13-2 -> libcudart.so.13

NVIDIA carries only a couple of minor toolkits per Ubuntu release, so a package
built against 13.0/13.2 is otherwise uninstallable on a host whose CUDA
repository publishes 13.1, 13.3 and 13.4.  The virtual package name cannot be
derived from the binary package name -- libcufft-13-3 provides libcufft.so.12,
not libcufft.so.13 -- so the mapping below is a table read off the published
package metadata.

The package payload is unpacked and repacked unchanged; only DEBIAN/control is
rewritten.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


RELATIONSHIP_FIELDS = (
    "Depends",
    "Pre-Depends",
    "Recommends",
    "Suggests",
)

DEFAULT_VERSION_REGEX = r"(^|[~+.:_-])24[.]04([~+.:_-]|$)|ubuntu[0-9]*[~+.:_-]?24[.]04"
VERSIONED_RELATIONSHIP_RE = re.compile(
    r"^(?P<prefix>\s*[^()\s]+(?:\s*:\s*[^()\s]+)?(?:\s*\[[^\]]+\])?\s*)"
    r"\((?P<operator><<|<=|=|>=|>>)\s*(?P<version>[^)]+)\)"
    r"(?P<suffix>\s*)$"
)

CUDA_MAJOR = "13"

# Binary package name with its "-<major>-<minor>" suffix removed, mapped to the
# virtual package every minor toolkit of that component provides.  Taken from
# apt-cache dumpavail over NVIDIA's ubuntu2404/ubuntu2604 repositories; only
# components that actually declare such a Provides appear here, which is why
# metapackages (cuda-libraries-13-2) and libcufile-13-2 are absent.
CUDA_MAJOR_VIRTUAL_PACKAGES = {
    "cuda-cudart": "libcudart.so.13",
    "cuda-cudart-dev": "libcudart.so.13-dev",
    "cuda-cupti": "libcupti.so.13",
    "cuda-cupti-dev": "libcupti.so.13-dev",
    "cuda-driver-dev": "libcuda.so.13-dev",
    "cuda-nvrtc": "libnvrtc.so.13",
    "cuda-nvrtc-dev": "libnvrtc.so.13-dev",
    "cuda-opencl": "libopencl.so.1",
    "cuda-opencl-dev": "libopencl.so.1-dev",
    "libcublas": "libcublas.so.13",
    "libcublas-dev": "libcublas.so.13-dev",
    "libcufft": "libcufft.so.12",
    "libcufft-dev": "libcufft.so.12-dev",
    "libcufile-dev": "libcufile.so.0-dev",
    "libcuobjclient-dev": "libcuobjclient.so.0-dev",
    "libcurand": "libcurand.so.10",
    "libcurand-dev": "libcurand.so.10-dev",
    "libcusolver": "libcusolver.so.12",
    "libcusolver-dev": "libcusolver.so.12-dev",
    "libcusparse": "libcusparse.so.12",
    "libcusparse-dev": "libcusparse.so.12-dev",
    "libnpp": "libnpp.so.13",
    "libnpp-dev": "libnpp.so.13-dev",
    "libnvfatbin": "libnvfatbin.so.13",
    "libnvfatbin-dev": "libnvfatbin.so.13-dev",
    "libnvjitlink": "libnvjitlink.so.13",
    "libnvjitlink-dev": "libnvjitlink.so.13-dev",
    "libnvjpeg": "libnvjpeg.so.13",
    "libnvjpeg-dev": "libnvjpeg.so.13-dev",
}

# The trailing lookahead keeps "cuda-cudart-13-0x" from being treated as a
# pinned name; the architecture qualifier and any build profile stay in "rest".
CUDA_MINOR_PACKAGE_RE = re.compile(
    r"^(?P<space>\s*)(?P<name>[a-z0-9][a-z0-9+.-]*?)"
    rf"-{CUDA_MAJOR}-[0-9]+(?![a-z0-9+.-])(?P<rest>.*)$",
    re.DOTALL,
)


def run(command):
    subprocess.run(command, check=True)


def unfold_control_fields(text):
    fields = []
    current_name = None
    current_lines = []
    for line in text.splitlines(keepends=True):
        if line.startswith((" ", "\t")) and current_name is not None:
            current_lines.append(line)
            continue
        if current_name is not None:
            fields.append((current_name, current_lines))
        current_lines = [line]
        current_name = line.split(":", 1)[0] if ":" in line else None
    if current_name is not None:
        fields.append((current_name, current_lines))
    return fields


def split_relationships(value):
    parts = []
    start = 0
    depth = 0
    for index, char in enumerate(value):
        if char == "(":
            depth += 1
        elif char == ")" and depth:
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(value[start:index])
            start = index + 1
    parts.append(value[start:])
    return parts


def relax_cuda_minor_pin(alternative):
    match = CUDA_MINOR_PACKAGE_RE.match(alternative)
    if not match:
        return alternative, False
    virtual_package = CUDA_MAJOR_VIRTUAL_PACKAGES.get(match.group("name"))
    if virtual_package is None:
        return alternative, False
    return f"{match.group('space')}{virtual_package}{match.group('rest')}", True


def relax_alternative(alternative, version_regex, relax_cuda_minor_pins):
    changed = False
    if relax_cuda_minor_pins:
        alternative, changed = relax_cuda_minor_pin(alternative)
    match = VERSIONED_RELATIONSHIP_RE.match(alternative)
    if match and version_regex.search(match.group("version")):
        alternative = f"{match.group('prefix').rstrip()}{match.group('suffix')}"
        changed = True
    return alternative, changed


def relax_relationship_value(value, version_regex, relax_cuda_minor_pins):
    changed = False
    relationships = []
    for relationship in split_relationships(value):
        alternatives = []
        seen = set()
        for alternative in relationship.split("|"):
            relaxed, alternative_changed = relax_alternative(
                alternative, version_regex, relax_cuda_minor_pins
            )
            changed = changed or alternative_changed
            relaxed = relaxed.strip()
            # Two minor pins of the same component collapse onto one virtual
            # package, so the alternative would otherwise be repeated.
            if not relaxed or relaxed in seen:
                changed = True
                continue
            seen.add(relaxed)
            alternatives.append(relaxed)
        if alternatives:
            relationships.append(" | ".join(alternatives))
    return ", ".join(relationships), changed


def format_control_field(name, value):
    # Keep relationship fields readable and valid. Debian control continuation
    # lines start with one space.
    if name in RELATIONSHIP_FIELDS:
        return f"{name}: {value.strip()}\n"
    return f"{name}:{value}"


def rewrite_control(control_path, version_regex, relax_cuda_minor_pins):
    text = control_path.read_text(encoding="utf-8")
    output = []
    changed_fields = []

    for name, lines in unfold_control_fields(text):
        if name not in RELATIONSHIP_FIELDS:
            output.extend(lines)
            continue
        first_line = lines[0]
        value = first_line.split(":", 1)[1] + "".join(lines[1:])
        value = re.sub(r"\n[ \t]*", " ", value).strip()
        new_value, changed = relax_relationship_value(
            value, version_regex, relax_cuda_minor_pins
        )
        if changed:
            changed_fields.append(name)
        output.append(format_control_field(name, new_value))

    if not changed_fields:
        return []
    control_path.write_text("".join(output), encoding="utf-8")
    return changed_fields


def default_output_path(input_deb):
    stem = input_deb.name[:-4] if input_deb.name.endswith(".deb") else input_deb.name
    return input_deb.with_name(f"{stem}.relaxed.deb")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Relax selected version and CUDA minor-toolkit pins in Debian "
        "package relationship fields."
    )
    parser.add_argument("input_deb", type=Path, help="Input .deb file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output .deb file. Defaults to INPUT.relaxed.deb.",
    )
    parser.add_argument(
        "--version-regex",
        default=DEFAULT_VERSION_REGEX,
        help="Regex matched against dependency version strings to remove. "
        "Default matches Ubuntu 24.04 package tags.",
    )
    parser.add_argument(
        "--keep-cuda-minor-pins",
        action="store_true",
        help=f"Leave dependencies on a specific CUDA {CUDA_MAJOR}.x minor toolkit "
        "alone instead of rewriting them onto the minor-independent virtual "
        "package the component provides.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite the output file if it already exists.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    input_deb = args.input_deb.resolve()
    output_deb = (args.output or default_output_path(input_deb)).resolve()
    version_regex = re.compile(args.version_regex)

    if not input_deb.is_file():
        print(f"ERROR: input package not found: {input_deb}", file=sys.stderr)
        return 1
    if output_deb.exists() and not args.force:
        print(f"ERROR: output package already exists: {output_deb}", file=sys.stderr)
        print("Pass --force to overwrite it.", file=sys.stderr)
        return 1
    if shutil.which("dpkg-deb") is None:
        print("ERROR: dpkg-deb is required.", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="relax-deb-deps.") as work_dir:
        package_root = Path(work_dir) / "package"
        run(["dpkg-deb", "-R", str(input_deb), str(package_root)])

        control_path = package_root / "DEBIAN" / "control"
        if not control_path.is_file():
            print(f"ERROR: control file not found in {input_deb}", file=sys.stderr)
            return 1

        changed_fields = rewrite_control(
            control_path, version_regex, not args.keep_cuda_minor_pins
        )
        output_deb.parent.mkdir(parents=True, exist_ok=True)
        if output_deb.exists():
            output_deb.unlink()
        run(["dpkg-deb", "--build", "--root-owner-group", str(package_root), str(output_deb)])

    if changed_fields:
        fields = ", ".join(sorted(set(changed_fields)))
        print(f"Rewrote {fields} in {output_deb}")
    else:
        print(f"No matching dependency pins found; copied metadata into {output_deb}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
