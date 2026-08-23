#!/usr/bin/env python3
"""Relax dependency version constraints in a Debian package.

By default this removes relationship versions that carry Ubuntu 24.04-specific
package tags, such as:

  libfoo (= 23.0.4-0ubuntu1~24.04.1) -> libfoo

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


def relax_alternative(alternative, version_regex):
    match = VERSIONED_RELATIONSHIP_RE.match(alternative)
    if not match:
        return alternative, False
    if not version_regex.search(match.group("version")):
        return alternative, False
    return f"{match.group('prefix').rstrip()}{match.group('suffix')}", True


def relax_relationship_value(value, version_regex):
    changed = False
    relationships = []
    for relationship in split_relationships(value):
        alternatives = []
        for alternative in relationship.split("|"):
            relaxed, alternative_changed = relax_alternative(alternative, version_regex)
            changed = changed or alternative_changed
            alternatives.append(relaxed)
        relationships.append(" |".join(alternatives))
    return ",".join(relationships), changed


def format_control_field(name, value):
    # Keep relationship fields readable and valid. Debian control continuation
    # lines start with one space.
    if name in RELATIONSHIP_FIELDS:
        return f"{name}: {value.strip()}\n"
    return f"{name}:{value}"


def rewrite_control(control_path, version_regex):
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
        new_value, changed = relax_relationship_value(value, version_regex)
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
        description="Remove selected version constraints from Debian package relationship fields."
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

        changed_fields = rewrite_control(control_path, version_regex)
        output_deb.parent.mkdir(parents=True, exist_ok=True)
        if output_deb.exists():
            output_deb.unlink()
        run(["dpkg-deb", "--build", "--root-owner-group", str(package_root), str(output_deb)])

    if changed_fields:
        fields = ", ".join(sorted(set(changed_fields)))
        print(f"Rewrote {fields} in {output_deb}")
    else:
        print(f"No matching dependency versions found; copied metadata into {output_deb}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
