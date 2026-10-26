#!/usr/bin/env python3
"""Frozen descriptor metadata probes, including replacement and helper limits."""

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from WebJobTest import plan, request


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="hstream-card-probe-") as temporary:
        root = Path(temporary)
        for case in (
            "unauthorized",
            "ambiguous",
            "identity",
            "final-link",
            "parent-link",
            "probe-failure",
            "output-limit",
            "replacement",
            "cancel",
            "timeout",
        ):
            directory = root / case
            directory.mkdir()
            source = directory / "published"
            source.mkdir()
            archive = source / "archive.mp4"
            archive.write_bytes(b"original archive")
            info = archive.stat()
            identity = {
                key: str(getattr(info, "st_" + key))
                for key in ("dev", "ino", "size", "mtime_ns", "ctime_ns")
            }
            job = directory / "job"
            job.mkdir()
            path = request(job, "highlight-card-preview", {"plan": plan()})
            value = json.loads(path.read_text())
            value["authorized_media"] = [str(archive)]
            value["runtime"] = {
                "archive_probe": {"path": str(archive), "identity": identity}
            }
            if case == "unauthorized":
                value["authorized_media"] = [str(directory / "other.mp4")]
            elif case == "ambiguous":
                value["authorized_media"].append(str(directory / "other.mp4"))
            elif case == "identity":
                identity["ino"] = "0"
            elif case in {"final-link", "parent-link"}:
                if case == "final-link":
                    alias = directory / "alias.mp4"
                    alias.symlink_to(archive)
                else:
                    alias_dir = directory / "alias"
                    alias_dir.symlink_to(source, target_is_directory=True)
                    alias = alias_dir / "archive.mp4"
                value["runtime"]["archive_probe"]["path"] = str(alias)
                value["authorized_media"] = [str(alias)]
            path.write_text(json.dumps(value))
            helpers = directory / "bin"
            helpers.mkdir()
            marker = directory / "probe-observed"
            script = helpers / "ffprobe"
            script.write_text(
                "#!" + sys.executable + "\n"
                "import json,sys,time\nfrom pathlib import Path\n"
                f"case={case!r}; archive=Path({str(archive)!r})\n"
                "assert sys.argv[sys.argv.index('-protocol_whitelist')+1]=='file'\n"
                "assert sys.argv[sys.argv.index('-enable_drefs')+1]=='0'\n"
                "assert sys.argv[sys.argv.index('-use_absolute_path')+1]=='0'\n"
                "if case=='replacement':\n"
                " archive.rename(archive.with_suffix('.old')); archive.write_bytes(b'replacement')\n"
                f"Path({str(marker)!r}).write_bytes(Path(sys.argv[-1]).read_bytes())\n"
                "if case=='probe-failure': sys.exit(1)\n"
                "if case=='output-limit': print('x'*2097152); sys.exit(0)\n"
                "if case in {'cancel','timeout'}: time.sleep(60)\n"
                "print(json.dumps({'streams':[{'codec_type':'video','codec_name':'h264',"
                "'width':320,'height':240,'r_frame_rate':'30/1','pix_fmt':'yuv420p'}],"
                "'format':{'duration':'1'}}))\n"
            )
            script.chmod(0o700)
            started = time.monotonic()
            child = subprocess.Popen(
                [str(binary), "--request", str(path)],
                env={
                    **os.environ,
                    "PATH": str(helpers) + os.pathsep + os.environ["PATH"],
                },
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if case == "cancel":
                deadline = time.monotonic() + 10
                while (
                    not marker.exists()
                    and child.poll() is None
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.02)
                assert marker.exists(), "The confined probe did not start"
                child.terminate()
            stdout, stderr = child.communicate(timeout=40)
            assert child.returncode == (130 if case == "cancel" else 1), (
                case,
                stdout,
                stderr,
            )
            receipt = json.loads((job / "operation-result.json").read_text())
            assert (
                receipt["state"] == ("cancelled" if case == "cancel" else "failed")
                and not receipt["artifacts"]
            ), (
                case,
                receipt,
            )
            if case in {
                "unauthorized",
                "ambiguous",
                "identity",
                "final-link",
                "parent-link",
            }:
                assert not marker.exists(), "Rejected archives must not reach ffprobe"
            else:
                assert marker.read_bytes() == b"original archive"
            if case == "replacement":
                assert "identity changed" in receipt["error"]
            if case == "timeout":
                assert 29 <= time.monotonic() - started < 38
            elif case == "cancel":
                assert time.monotonic() - started < 10


if __name__ == "__main__":
    main()
