#!/usr/bin/env python3
"""Synchronize the portable workflow kernel into a paired web checkout."""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-root", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / "hstream_workflow"
    destination = args.web_root / "tools/webapp/django_app/stream/workflow_kernel"
    failed = []
    for name in ("schema.py", "graph.py"):
        expected = (source / name).read_bytes()
        target = destination / name
        if args.check:
            if not target.is_file() or target.read_bytes() != expected:
                failed.append(str(target))
        else:
            destination.mkdir(parents=True, exist_ok=True)
            target.write_bytes(expected)
    if failed:
        raise SystemExit("Workflow kernel differs: " + ", ".join(failed))


if __name__ == "__main__":
    main()
