#!/usr/bin/env bash
set -euo pipefail
# Requires a real X11 desktop, GPU, and an already oriented/synchronized game.
source_game=${1:?usage: test_stitching_match_editor_e2e.sh SOURCE_GAME_DIRECTORY [ARTIFACT_DIRECTORY]}
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact_dir=${2:-${HOME}/Videos/hstream-match-editor-e2e-$(date +%Y%m%d-%H%M%S)}
if [[ -e ${artifact_dir} ]]; then
  echo "Artifact directory already exists: ${artifact_dir}" >&2
  exit 2
fi
: "${DISPLAY:?This test requires DISPLAY and a working X11 desktop}"
mkdir -p "${artifact_dir}"
artifact_dir=$(realpath "${artifact_dir}")
sandbox_game=${artifact_dir}/games/fixture
mkdir -p "${sandbox_game}"
python3 - "${source_game}" "${sandbox_game}" <<'PY'
import os, pathlib, shutil, subprocess, sys
source, destination = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
if not (source / 'config.yaml').is_file():
    raise SystemExit('Source game requires config.yaml')
shutil.copyfile(source / 'config.yaml', destination / 'config.yaml')
# Video inputs are read-only. Hard links keep a valid in-game regular-file
# identity without duplicating recordings; the artifact directory must be on
# the same filesystem. All mutable config/sidecars and image bundles are copies.
for entry in source.iterdir():
    if entry.suffix.lower() in ('.mp4', '.mkv', '.mov', '.avi'):
        os.link(entry.resolve(), destination / entry.name)
    elif entry.suffix.lower() in ('.json', '.csv'):
        shutil.copyfile(entry.resolve(), destination / entry.name)
    elif entry.is_dir() and entry.name.startswith('cam') and entry.name[3:].isdigit():
        (destination / entry.name).mkdir()
        for child in entry.iterdir():
            if child.is_file():
                if child.suffix.lower() in ('.mp4', '.mkv', '.mov', '.avi'):
                    os.link(child.resolve(), destination / entry.name / child.name)
                else:
                    shutil.copyfile(child.resolve(), destination / entry.name / child.name)
inputs = source / 'player-frame-inputs'
if inputs.is_dir():
    subprocess.run(['cp', '-aL', '--reflink=auto', str(inputs), str(destination / inputs.name)], check=True)
(destination / '.match-editor-e2e-sandbox').touch()
PY
cd "${repo_root}"
bazel_output=${HSTREAM_MATCH_EDITOR_BAZEL_OUTPUT_BASE:-${HOME}/.bazel-modify-matches}
bazelisk --output_base="${bazel_output}" build --symlink_prefix=/ --config=opt --cpu=k8 --config=blackwell \
  //src/apps/hstream-ui:stitching_match_editor_e2e //src/apps/hstream-cli:hstream-cli
QT_QPA_PLATFORM=xcb "${bazel_output}/execroot/kstream/bazel-out/k8-opt/bin/src/apps/hstream-ui/stitching_match_editor_e2e" \
  "${sandbox_game}" "${repo_root}" "${artifact_dir}" \
  "${bazel_output}/execroot/kstream/bazel-out/k8-opt/bin/src/apps/hstream-cli/hstream-cli"
echo "Passed; logs and screenshots: ${artifact_dir}"
