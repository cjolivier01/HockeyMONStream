"""Command interface to durable native workflow execution."""

import argparse
import json
import os
import shlex
import sys
import time
from pathlib import Path

from .controller import Controller, serve
from .files import private_directory, read_json, sync_directory, write_json
from .schema import WorkflowError, digest, require, validate_plan


def export_plan(registry, plan_path, output):
    controller = Controller(registry)
    plan = validate_plan(read_json(plan_path))
    for key, profile in plan["profiles"].items():
        require(
            key in controller.config["profiles"]
            and digest(profile) == digest(controller.config["profiles"][key]),
            "Export profile is not installed",
        )
    output = Path(output).absolute()
    private_directory(output.parent)
    sealed = output.with_name(output.name + ".plan.json")
    write_json(sealed, plan, exclusive=True)
    launcher = str(Path(__file__).resolve().parents[1] / "hstream-workflow")
    contents = (
        "#!/usr/bin/env bash\n"
        "# Workflow controller: run directly or in a CPU-only Slurm allocation.\n"
        "# GPU resources are requested independently for each ready task.\n"
        "set -euo pipefail\numask 077\n"
        "exec "
        + shlex.join(
            [
                launcher,
                "run",
                "--registry",
                str(Path(registry).absolute()),
                "--plan",
                str(sealed),
            ]
        )
        + ' "$@"\n'
    )
    try:
        with output.open("x", encoding="utf-8") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        output.chmod(0o700)
        sync_directory(output.parent)
    except OSError:
        sealed.unlink(missing_ok=True)
        raise
    return {
        "script": str(output),
        "plan": str(sealed),
        "workflow_id": plan["workflow_id"],
        "plan_digest": digest(plan),
    }


def parser():
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    for name in (
        "profiles",
        "submit",
        "status",
        "cancel",
        "retry",
        "serve",
        "run",
        "export",
        "log",
        "events",
        "reconcile",
    ):
        command = commands.add_parser(name)
        command.add_argument("--registry", required=True, type=Path)
        if name in {"submit", "run", "export"}:
            command.add_argument("--plan", required=True, type=Path)
        if name in {"submit", "run"}:
            command.add_argument("--game-dir", type=Path)
        if name in {"status", "cancel", "retry", "log", "events", "reconcile"}:
            command.add_argument("--workflow", required=True)
        if name in {"cancel", "retry"}:
            command.add_argument("--epoch", required=True, type=int)
        if name == "retry":
            command.add_argument("--stage", required=True, action="append")
        if name == "log":
            command.add_argument("--stage", required=True)
            command.add_argument("--attempt")
        if name == "events":
            command.add_argument("--stage")
        if name == "export":
            command.add_argument("--output", required=True, type=Path)
        if name == "serve":
            command.add_argument("--once", action="store_true")
    supervise = commands.add_parser("_supervise", help=argparse.SUPPRESS)
    supervise.add_argument("--root", required=True, type=Path)
    return root


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.command == "_supervise":
            from .supervisor import execute

            return execute(args.root)
        if args.command == "serve":
            serve(args.registry, args.once)
            return 0
        controller = Controller(args.registry)
        if args.command == "profiles":
            result = controller.profiles()
        elif args.command in {"submit", "run"}:
            plan = read_json(args.plan)
            game = args.game_dir or plan.get("source_identity", {}).get(
                "game_directory"
            )
            require(
                game is not None, "Specify --game-dir or a sealed source game_directory"
            )
            result = controller.register(plan, game)
            if args.command == "run":
                while True:
                    controller.tick(plan["workflow_id"])
                    result = controller.status(plan["workflow_id"])
                    if (
                        result["state"] in {"succeeded", "failed", "cancelled"}
                        and "controller_error" not in result
                    ):
                        break
                    if result["state"] == "recovery" or "controller_error" in result:
                        print(json.dumps(result, sort_keys=True))
                        return 2
                    time.sleep(controller.defaults["poll_interval_ms"] / 1000)
                print(json.dumps(result, sort_keys=True))
                return 0 if result["state"] == "succeeded" else 1
        elif args.command == "export":
            result = export_plan(args.registry, args.plan, args.output)
        elif args.command == "status":
            result = controller.status(args.workflow)
        elif args.command == "cancel":
            result = controller.cancel(args.workflow, args.epoch)
        elif args.command == "retry":
            result = controller.retry(args.workflow, args.stage, args.epoch)
        elif args.command == "events":
            result = controller.events(args.workflow, args.stage)
        elif args.command == "log":
            result = controller.log(args.workflow, args.stage, args.attempt)
        else:
            result = controller.tick(args.workflow)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, WorkflowError, ValueError) as exc:
        print(json.dumps({"error": str(exc)[:8192]}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
