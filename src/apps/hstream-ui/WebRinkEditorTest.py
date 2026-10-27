#!/usr/bin/env python3
"""Exercise the private adapter with real Hugin camera transforms, without a GPU."""

import json
import pathlib
import subprocess
import sys
import tempfile


def main(binary):
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix="hstream-rink-editor-") as temporary:
        root = pathlib.Path(temporary)
        project = (
            'p f1 w400 h200 v140 n"PNG"\n'
            'm g1 i0 f0 m2 p0.00784314\n'
            'i w100 h100 f0 v90 y-30 p0 r0 n"left.png"\n'
            'i w100 h100 f0 v90 y30 p0 r0 n"right.png"\n'
        )
        value = {
            "pto": project,
            "sourceDirectory": str(root),
            "method": "posts",
            "points": [[[x, y] for x in (10, 49.5, 89) for y in (20, 70)]] * 2,
            "angles": [5, 0, 0],
            "publishedAngles": [0, 0, 0],
        }

        def invoke(expected=0):
            path = root / "request.json"
            path.write_text(json.dumps(value))
            result = subprocess.run(
                [binary, "--rink-editor", str(path)], capture_output=True, text=True, timeout=15
            )
            assert result.returncode == expected, result.stderr
            return json.loads(result.stdout) if expected == 0 else result.stderr

        for rotation in ([0, 0, 0], [23, -31, 7]):
            value.pop("rays", None)
            (root / "original.pto").write_text(project)
            subprocess.run(
                ["pano_modify", "--rotate=" + ",".join(map(str, rotation)),
                 "--output=" + str(root / "rotated.pto"), str(root / "original.pto")],
                check=True, capture_output=True, timeout=15,
            )
            value["pto"] = (root / "rotated.pto").read_text()
            value["publishedAngles"] = rotation
            prepared = invoke()
            assert prepared["imageSizes"] == [[100, 100], [100, 100]]
            (root / "sphere.pto").write_text(prepared["sphere"])
            rays = subprocess.run(
                ["pano_trafo", str(root / "sphere.pto")], input=prepared["input"],
                capture_output=True, text=True, check=True, timeout=15,
            ).stdout
            value["rays"] = rays
            estimated = invoke()
            assert estimated["inlierCount"] == estimated["postCount"] == 6, estimated
            assert all(abs(a - b) < 1e-5 for a, b in zip(estimated["angles"], [5, 0, 0])), estimated
        value["pto"] = project.replace('n"left.png"', 'n"/outside/left.png"')
        assert "image" in invoke(1).lower()
        value["pto"] = project
        value["points"][0][0] = [100, 20]
        assert "image" in invoke(1).lower()
        value["points"][0][0] = [10, 20]
        value["method"] = "corners"
        assert "pairs" in invoke(1).lower()
    print("Web rink editor real-Hugin tests passed")


if __name__ == "__main__":
    main(sys.argv[1])
