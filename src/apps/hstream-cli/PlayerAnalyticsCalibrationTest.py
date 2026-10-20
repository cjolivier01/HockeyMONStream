"""Exercise runner mapping order without decoding video or loading a model."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    runner = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="team-calibration-") as directory:
        root = Path(directory)
        game = root / "games" / "test"
        game.mkdir(parents=True)
        saved = game / "config.yaml"
        saved.write_text("team: {enabled: true}\n")
        environment = dict(os.environ)
        environment.update(
            HM_GAME_DIR=str(root / "games"),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_CACHE_HOME=str(root / "cache"),
            XDG_STATE_HOME=str(root / "state"),
        )
        for stage, arguments in ((0, ["--stitching-calibration-only"]), (-1, []), (0, [])):
            pipeline = root / f"pipeline-{stage}.yaml"
            # The unsupported source is a sentinel after model preparation.
            # Reaching it proves saved team.enabled survived config loading
            # without re-enabling analytics in the temporary graph. Neither
            # decoding nor a usable GPU/model is needed to reach this error.
            pipeline.write_text(
                f"application: {{stage: {stage}, complete-configuration: 0, enable-perf-measurement: 0}}\n"
                "primary-gie: {enable: 0}\n"
                "tracker: {enable: 0}\n"
                "source0: {enable: 1, type: 0}\n"
                "sink0: {enable: 1, type: 1, sink-id: 0}\n"
            )
            result = subprocess.run(
                [runner, "-c", str(pipeline), "-g", "test", *arguments],
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
            )
            suppressed = stage < 0 or bool(arguments)
            expected = "Source type not yet implemented!" if suppressed else "Player analytics and ReID require"
            assert result.returncode != 0 and expected in result.stdout, result.stdout
            if suppressed:
                assert "Player analytics and ReID require" not in result.stdout, result.stdout
            assert saved.read_text() == "team: {enabled: true}\n", "Calibration changed the saved preference"
    print("Saved team enable stays suppressed through calibration mappings")


if __name__ == "__main__":
    main()
