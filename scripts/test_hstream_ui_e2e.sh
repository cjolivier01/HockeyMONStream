#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 [--x11-preview] GAME_ID [ARTIFACT_DIR]" >&2
  echo "Runs the real hstream-ui button workflow in an isolated game sandbox." >&2
}

verify_x11_preview=0
if [[ ${1:-} == --x11-preview ]]; then
  verify_x11_preview=1
  shift
fi
if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

game_id=$1
source_root=${HM_GAME_DIR:-${HOME}/Videos}
source_game=${source_root}/${game_id}
if [[ ! -d ${source_game} ]]; then
  echo "Game directory does not exist: ${source_game}" >&2
  exit 2
fi
if [[ ! -s ${source_game}/config.yaml ]]; then
  echo "The UI E2E test requires a non-empty ${source_game}/config.yaml" >&2
  exit 2
fi
if [[ ! -s ${source_game}/panorama.tif ]]; then
  echo "The UI E2E test requires configured stitching at ${source_game}/panorama.tif" >&2
  exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
  echo "curl is required to complete the private scoreboard selector during the automated run" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
timestamp=$(date +%Y%m%d-%H%M%S)
artifact_dir=${2:-${repo_root}/test-artifacts/hstream-ui/${game_id}-${timestamp}}
if [[ -e ${artifact_dir} ]]; then
  echo "Artifact directory already exists: ${artifact_dir}" >&2
  exit 2
fi
sandbox_root=${artifact_dir}/game-root
sandbox_game=${sandbox_root}/${game_id}
output_root=${artifact_dir}/encoded-output
mkdir -p "${sandbox_game}" "${output_root}"

while IFS= read -r -d '' entry; do
  name=$(basename "${entry}")
  case ${name} in
    config.yaml)
      cp --reflink=auto -- "${entry}" "${sandbox_game}/config.yaml"
      ;;
    .hstream-ui)
      mkdir -p "${sandbox_game}/.hstream-ui"
      ;;
    .hstream-*.lock)
      ;;
    cam[0-9]*)
      mkdir -p "${sandbox_game}/${name}"
      while IFS= read -r -d '' camera_entry; do
        camera_name=$(basename "${camera_entry}")
        case ${camera_name} in
          *.mp4|*.MP4|*.mkv|*.MKV|*.mov|*.MOV|*.avi|*.AVI)
            ln -s -- "${camera_entry}" "${sandbox_game}/${name}/${camera_name}"
            ;;
          *)
            cp -aL --reflink=auto -- "${camera_entry}" "${sandbox_game}/${name}/${camera_name}"
            ;;
        esac
      done < <(find "${entry}" -mindepth 1 -maxdepth 1 -print0)
      ;;
    *.mp4|*.MP4|*.mkv|*.MKV|*.mov|*.MOV|*.avi|*.AVI)
      ln -s -- "${entry}" "${sandbox_game}/${name}"
      ;;
    *)
      cp -aL --reflink=auto -- "${entry}" "${sandbox_game}/${name}"
      ;;
  esac
done < <(find "${source_game}" -mindepth 1 -maxdepth 1 -print0)

cd "${repo_root}"
bazelisk build --config=opt --cpu=k8 \
  //src/apps/hstream-ui:hstream_ui_test \
  //src/apps/hstream-ui:hstream_ui_visual_verifier \
  //src/apps/pipeline-app:hstream-cli \
  //src/gst-plugins/gst-dsxvideoconvert:libgstdsxvideoconvert.so \
  //src/gst-plugins/gst-fieldmask:libnvdsgst_dsfieldmask.so \
  //src/gst-plugins/gst-playtracker:libgstplaytracker.so \
  //src/gst-plugins/gst-videoprep:libnvdsgst_videoprep.so \
  //src/libs/nvdsinfer_custom_impl_Yolo:nvdsinfer_custom_impl_Yolo

qt_platform=offscreen
if [[ ${verify_x11_preview} == 1 ]]; then
  qt_platform=xcb
  if [[ -z ${DISPLAY:-} ]]; then
    echo "--x11-preview requires DISPLAY to name an active X11 display" >&2
    exit 2
  fi
fi
e2e_record_default=6000

e2e_env=(
  "QT_QPA_PLATFORM=${qt_platform}"
  "HM_GAME_DIR=${sandbox_root}"
  "HM_OUTPUT_WORK_DIR=${output_root}"
  "HSTREAM_SCOREBOARD_BROWSER=/bin/true"
  "HSTREAM_UI_E2E_GAME_ID=${game_id}"
  "HSTREAM_UI_E2E_PREPARE_GAME=1"
  "HSTREAM_UI_E2E_REQUIRE_SCOREBOARD_SELECTOR=1"
  "HSTREAM_UI_E2E_RUN_MODE=program"
  "HSTREAM_UI_E2E_TIMEOUT_MS=${HSTREAM_UI_E2E_TIMEOUT_MS:-180000}"
  "HSTREAM_UI_E2E_RECORD_MS=${HSTREAM_UI_E2E_RECORD_MS:-${e2e_record_default}}"
  "HSTREAM_UI_E2E_ARTIFACT_DIR=${artifact_dir}"
  "HSTREAM_UI_E2E_VISUAL_VERIFIER=${repo_root}/bazel-bin/src/apps/hstream-ui/hstream_ui_visual_verifier"
)
if [[ ${verify_x11_preview} == 1 ]]; then
  e2e_env+=("HSTREAM_UI_E2E_VERIFY_X11_PREVIEW=1")
fi

env -u HM_NO_SCOREBOARD "${e2e_env[@]}" \
  "${repo_root}/bazel-bin/src/apps/hstream-ui/hstream_ui_test"

echo "HStream UI E2E passed. Artifacts: ${artifact_dir}"
