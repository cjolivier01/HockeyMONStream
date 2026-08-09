#!/bin/bash
# Install the local DeepStream and HStream Debian artifacts on a clean Ubuntu
# host, including the NVIDIA repositories that provide their CUDA/TensorRT
# dependencies.
set -euo pipefail

CUDA_COMPAT_REPOSITORY='https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/'
CUDA_COMPAT_KEYRING='/usr/share/keyrings/hstream-cuda-ubuntu2404-compat.gpg'
CUDA_COMPAT_SOURCE='/etc/apt/sources.list.d/hstream-cuda-ubuntu2404-x86_64.list'
CUDA_LEGACY_COMPAT_SOURCE='/etc/apt/sources.list.d/cuda-ubuntu2404-x86_64.list'

# Disable duplicate definitions of the Ubuntu 24 CUDA repository before one
# canonical installer-owned source is atomically published.  Disabling entries
# one file at a time cannot introduce a Signed-By conflict, so a power loss at
# any point leaves APT usable and a rerun can finish the transition.
#
# A root prefix is accepted so the exact production parser/rewriter can be
# exercised against an isolated APT tree by the Bazel regression test.
disable_cuda_compat_sources() {
  local apt_root="$1"
  local only_source="${2:-}"
  local source_path resolved_path extension output matches total_matches=0
  local -a candidates=()
  local -A visited=()

  if [[ -n "${only_source}" ]]; then
    candidates+=("${apt_root}${only_source}")
  else
    if [[ -f "${apt_root}/etc/apt/sources.list" ]]; then
      candidates+=("${apt_root}/etc/apt/sources.list")
    fi
    shopt -s nullglob
    candidates+=("${apt_root}/etc/apt/sources.list.d/"*.list)
    candidates+=("${apt_root}/etc/apt/sources.list.d/"*.sources)
    shopt -u nullglob
  fi

  for source_path in "${candidates[@]}"; do
    [[ -f "${source_path}" ]] || continue
    resolved_path="$(readlink -f "${source_path}")"
    [[ -n "${resolved_path}" && -f "${resolved_path}" ]] || continue
    if [[ -n "${visited[${resolved_path}]:-}" ]]; then continue; fi
    visited["${resolved_path}"]=1
    extension="${source_path##*.}"
    output="$(mktemp "${resolved_path}.hstream.XXXXXX")"
    matches="${output}.matches"

    if [[ "${extension}" == "list" || "${source_path}" == "${apt_root}/etc/apt/sources.list" ]]; then
      awk -v repository="${CUDA_COMPAT_REPOSITORY}" -v matches="${matches}" '
        function trim(value) {
          sub(/^[[:space:]]+/, "", value)
          sub(/[[:space:]]+$/, "", value)
          return value
        }
        function canonical(value, result) {
          result = tolower(value)
          sub(/^http:/, "https:", result)
          sub(/\/+$/, "", result)
          return result
        }
        BEGIN {
          canonical_repository = canonical(repository)
          count = 0
        }
        {
          line = $0
          if (line !~ /^[[:space:]]*deb(-src)?[[:space:]]/) {
            print line
            next
          }

          match(line, /^[[:space:]]*deb(-src)?[[:space:]]+/)
          prefix = substr(line, 1, RLENGTH)
          rest = substr(line, RLENGTH + 1)
          options = ""
          if (rest ~ /^\[/) {
            closing = index(rest, "]")
            if (closing == 0) {
              print line
              next
            }
            options = substr(rest, 2, closing - 2)
            rest = trim(substr(rest, closing + 1))
          }
          split(rest, fields, /[[:space:]]+/)
          uri = fields[1]
          if (canonical(uri) != canonical_repository) {
            print line
            next
          }

          remainder = substr(rest, length(uri) + 1)
          split(trim(remainder), repository_fields, /[[:space:]]+/)
          if (repository_fields[1] != "/") {
            print line
            next
          }
          print "# HStream disabled duplicate CUDA compatibility source: " line
          count++
        }
        END { print count > matches }
      ' "${resolved_path}" >"${output}" || {
        local status=$?
        rm -f "${output}" "${matches}"
        return "${status}"
      }
    else
      awk -v repository="${CUDA_COMPAT_REPOSITORY}" -v matches="${matches}" '
        function trim(value) {
          sub(/^[[:space:]]+/, "", value)
          sub(/[[:space:]]+$/, "", value)
          return value
        }
        function canonical(value, result) {
          result = tolower(value)
          sub(/^http:/, "https:", result)
          sub(/\/+$/, "", result)
          return result
        }
        function field_value(name,    value, i, current, line, field) {
          value = ""
          current = ""
          for (i = 1; i <= line_count; i++) {
            line = lines[i]
            if (line ~ /^[^[:space:]#][^:]*:/) {
              field = line
              sub(/:.*/, "", field)
              current = tolower(field)
              sub(/^[^:]*:[[:space:]]*/, "", line)
              if (current == name) value = line
            } else if (line ~ /^[[:space:]]+/ && current == name) {
              value = value " " trim(line)
            }
          }
          return trim(value)
        }
        function has_word(value, word,    count, words, i) {
          count = split(tolower(value), words, /[[:space:]]+/)
          for (i = 1; i <= count; i++) if (words[i] == word) return 1
          return 0
        }
        function supported_uris(value,    count, words, i, item) {
          count = split(value, words, /[[:space:]]+/)
          for (i = 1; i <= count; i++) {
            item = canonical(words[i])
            if (item == canonical_repository) continue
            if (item ~ /^https:\/\/developer[.]download[.]nvidia[.]com\/compute\/cuda\/repos\/ubuntu(2404|2604)\/x86_64$/) continue
            return 0
          }
          return 1
        }
        BEGIN {
          RS = ""
          ORS = "\n\n"
          canonical_repository = canonical(repository)
          count = 0
          failed = 0
        }
        {
          line_count = split($0, lines, /\n/)
          types = field_value("types")
          enabled = tolower(field_value("enabled"))
          uris = field_value("uris")
          suites = field_value("suites")
          target = enabled != "no" && (has_word(types, "deb") || has_word(types, "deb-src"))
          target = target && has_word(suites, "/")
          uri_count = split(uris, uri_words, /[[:space:]]+/)
          found_uri = 0
          for (uri_index = 1; uri_index <= uri_count; uri_index++) {
            if (canonical(uri_words[uri_index]) == canonical_repository) found_uri = 1
          }
          target = target && found_uri
          if (!target) {
            print $0
            next
          }
          if (!supported_uris(uris)) {
            print "ERROR: CUDA compatibility URI shares a Deb822 stanza with an unsupported repository" > "/dev/stderr"
            failed = 1
            print $0
            next
          }

          wrote_enabled = 0
          skip_continuation = 0
          for (i = 1; i <= line_count; i++) {
            line = lines[i]
            if (line ~ /^[^[:space:]#][^:]*:/) {
              field = line
              sub(/:.*/, "", field)
              lower_field = tolower(field)
              skip_continuation = 0
              if (lower_field == "enabled") {
                if (!wrote_enabled) printf "Enabled: no\n"
                wrote_enabled = 1
                skip_continuation = 1
                continue
              }
            } else if (line ~ /^[[:space:]]+/ && skip_continuation) {
              continue
            } else if (line !~ /^[[:space:]]+/) {
              skip_continuation = 0
            }
            printf "%s\n", line
          }
          if (!wrote_enabled) printf "Enabled: no\n"
          printf "# HStream disabled duplicate CUDA compatibility source\n"
          printf "\n"
          count++
        }
        END {
          print count > matches
          if (failed) exit 42
        }
      ' "${resolved_path}" >"${output}" || {
        local status=$?
        rm -f "${output}" "${matches}"
        return "${status}"
      }
    fi

    local file_matches
    file_matches="$(<"${matches}")"
    rm -f "${matches}"
    if [[ "${file_matches}" -gt 0 ]]; then
      backup_compat_source_path "${resolved_path}"
      chmod --reference="${resolved_path}" "${output}"
      chown --reference="${resolved_path}" "${output}"
      sync -d "${output}"
      mv -f "${output}" "${resolved_path}"
      sync -f "$(dirname "${resolved_path}")"
      total_matches=$((total_matches + file_matches))
    else
      rm -f "${output}"
    fi
  done
  DISABLED_CUDA_SOURCE_COUNT="${total_matches}"
}

# shellcheck disable=SC2034 # Exposed to the sourced behavior test.
DISABLED_CUDA_SOURCE_COUNT=0

compat_source_transition_dir=""
compat_source_transition_committed=0
declare -a compat_source_paths=()
declare -a compat_source_backups=()
declare -a compat_source_existed=()

begin_compat_source_transition() {
  [[ -z "${compat_source_transition_dir}" ]] || return 0
  compat_source_transition_dir="$(mktemp -d /tmp/hstream-cuda-source-transition.XXXXXX)"
}

backup_compat_source_path() {
  local path="$1"
  local index backup
  [[ -n "${compat_source_transition_dir}" ]] || return 0
  for index in "${!compat_source_paths[@]}"; do
    if [[ "${compat_source_paths[${index}]}" == "${path}" ]]; then return 0; fi
  done
  index="${#compat_source_paths[@]}"
  backup="${compat_source_transition_dir}/${index}"
  if [[ -e "${path}" || -L "${path}" ]]; then
    if ! cp -a -- "${path}" "${backup}"; then
      return 1
    fi
    compat_source_paths+=("${path}")
    compat_source_backups+=("${backup}")
    compat_source_existed+=(1)
  else
    compat_source_paths+=("${path}")
    compat_source_backups+=("${backup}")
    compat_source_existed+=(0)
  fi
}

restore_compat_source_transition() {
  local index path backup temporary
  [[ -n "${compat_source_transition_dir}" && "${compat_source_transition_committed}" -eq 0 ]] || return 0
  for ((index=${#compat_source_paths[@]} - 1; index >= 0; index--)); do
    path="${compat_source_paths[${index}]}"
    backup="${compat_source_backups[${index}]}"
    temporary="${path}.hstream-restore.$$.${index}"
    rm -f -- "${temporary}"
    if [[ "${compat_source_existed[${index}]}" -eq 1 ]]; then
      cp -a -- "${backup}" "${temporary}"
      if [[ -f "${temporary}" && ! -L "${temporary}" ]]; then sync -d "${temporary}"; fi
      mv -Tf "${temporary}" "${path}"
    else
      rm -f -- "${path}"
    fi
    sync -f "$(dirname "${path}")"
  done
}

commit_compat_source_transition() {
  compat_source_transition_committed=1
}

disable_installer_managed_cuda_sources() {
  local apt_root="$1"
  local managed_source="${apt_root}${CUDA_COMPAT_SOURCE}"
  if [[ -e "${managed_source}" || -L "${managed_source}" ]]; then
    backup_compat_source_path "${managed_source}"
    rm -f -- "${managed_source}"
    sync -f "$(dirname "${managed_source}")"
  fi
}

publish_cuda_compat_source() {
  local apt_root="$1"
  local target="${apt_root}${CUDA_COMPAT_SOURCE}"
  local temporary
  mkdir -p "$(dirname "${target}")"
  backup_compat_source_path "${target}"
  temporary="$(mktemp "${target}.XXXXXX")"
  if ! printf '%s\n' \
    "deb [arch=amd64 signed-by=${CUDA_COMPAT_KEYRING}] ${CUDA_COMPAT_REPOSITORY} /" \
    >"${temporary}"; then
    rm -f "${temporary}"
    return 1
  fi
  chmod 0644 "${temporary}"
  sync -d "${temporary}"
  mv -f "${temporary}" "${target}"
  sync -f "$(dirname "${target}")"
}

# Allow the behavior test to source the production implementation without
# entering the privileged installer workflow.
if [[ "${HSTREAM_INSTALLER_SOURCE_ONLY:-0}" == "1" ]]; then
  # shellcheck disable=SC2317 # exit is used when executed rather than sourced.
  return 0 2>/dev/null || exit 0
fi

HSTREAM_DEB=""
DEEPSTREAM_DEB=""
EXPECTED_DEEPSTREAM_VERSION="9.1.0-1+resolute2"
SIMULATE=0

usage() {
  cat <<'USAGE'
Usage:
  sudo ./install-hstream-deb \
    --deepstream-deb=/path/to/deepstream-9.1_9.1.0-1+resolute2_amd64.deb \
    --hstream-deb=/path/to/hstream_*_amd64.deb

Options:
  --deepstream-deb FILE  Local deepstream-9.1 release artifact.
  --hstream-deb FILE    Local HStream artifact for this Ubuntu release.
  --simulate             Configure repositories and only simulate apt install.
  -h, --help             Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deepstream-deb) DEEPSTREAM_DEB="$2"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
    --hstream-deb) HSTREAM_DEB="$2"; shift ;;
    --hstream-deb=*) HSTREAM_DEB="${1#*=}" ;;
    --simulate) SIMULATE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: run this installer as root (for example, with sudo)." >&2
  exit 1
fi
if [[ -z "${HSTREAM_DEB}" || -z "${DEEPSTREAM_DEB}" ]]; then
  echo "ERROR: --hstream-deb and --deepstream-deb are required." >&2
  usage >&2
  exit 1
fi

for deb in "${HSTREAM_DEB}" "${DEEPSTREAM_DEB}"; do
  if [[ ! -f "${deb}" ]]; then
    echo "ERROR: Debian artifact not found: ${deb}" >&2
    exit 1
  fi
done
HSTREAM_DEB="$(readlink -f "${HSTREAM_DEB}")"
DEEPSTREAM_DEB="$(readlink -f "${DEEPSTREAM_DEB}")"
if [[ "$(dpkg-deb -f "${HSTREAM_DEB}" Package)" != "hstream" ]]; then
  echo "ERROR: not an hstream package: ${HSTREAM_DEB}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Package)" != "deepstream-9.1" ]]; then
  echo "ERROR: not a deepstream-9.1 package: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi
if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Version)" != "${EXPECTED_DEEPSTREAM_VERSION}" ]]; then
  echo "ERROR: DeepStream ${EXPECTED_DEEPSTREAM_VERSION} is required: ${DEEPSTREAM_DEB}" >&2
  exit 1
fi

if [[ ! -r /etc/os-release ]]; then
  echo "ERROR: cannot identify the target operating system." >&2
  exit 1
fi
# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" ]]; then
  echo "ERROR: HStream Debian artifacts currently support Ubuntu only." >&2
  exit 1
fi
case "${VERSION_ID:-}" in
  24.04) CUDA_REPOSITORY=ubuntu2404 ;;
  26.04) CUDA_REPOSITORY=ubuntu2604 ;;
  *)
    echo "ERROR: unsupported Ubuntu release: ${VERSION_ID:-unknown} (expected 24.04 or 26.04)." >&2
    exit 1
    ;;
esac

HSTREAM_TARGET_UBUNTU="$(dpkg-deb -f "${HSTREAM_DEB}" X-HStream-Target-Ubuntu 2>/dev/null || true)"
if [[ "${HSTREAM_TARGET_UBUNTU}" != "${VERSION_ID}" ]]; then
  echo "ERROR: the selected HStream artifact targets Ubuntu ${HSTREAM_TARGET_UBUNTU:-unknown}, not ${VERSION_ID}." >&2
  exit 1
fi

HOST_ARCH="$(dpkg --print-architecture)"
for deb in "${HSTREAM_DEB}" "${DEEPSTREAM_DEB}"; do
  deb_arch="$(dpkg-deb -f "${deb}" Architecture)"
  if [[ "${deb_arch}" != "${HOST_ARCH}" ]]; then
    echo "ERROR: ${deb} targets ${deb_arch}, but this host is ${HOST_ARCH}." >&2
    exit 1
  fi
done

export DEBIAN_FRONTEND=noninteractive
keyring_deb=""
compat_keyring_deb=""
native_dir=""
compat_dir=""
combined_keyring=""
compat_keyring_target_temp=""
transition_dir=""
cleanup() {
  local status=$?
  set +e
  if [[ "${status}" -ne 0 && "${compat_source_transition_committed}" -eq 0 ]]; then
    restore_compat_source_transition
  fi
  if [[ -n "${keyring_deb}" ]]; then rm -f "${keyring_deb}"; fi
  if [[ -n "${compat_keyring_deb}" ]]; then rm -f "${compat_keyring_deb}"; fi
  if [[ -n "${native_dir}" ]]; then rm -rf "${native_dir}"; fi
  if [[ -n "${compat_dir}" ]]; then rm -rf "${compat_dir}"; fi
  if [[ -n "${combined_keyring}" ]]; then rm -f "${combined_keyring}"; fi
  if [[ -n "${compat_keyring_target_temp}" ]]; then rm -f "${compat_keyring_target_temp}"; fi
  if [[ -n "${transition_dir}" ]]; then rm -rf "${transition_dir}"; fi
  if [[ -n "${compat_source_transition_dir}" ]]; then rm -rf "${compat_source_transition_dir}"; fi
  return "${status}"
}
trap cleanup EXIT

# Older installers could conflict with a pre-existing source before reaching
# repair code.  Remove the uniquely owned HStream entry and disable only the
# matching line in NVIDIA's legacy conffile before the first APT update.  A
# normal failure restores both; a crash leaves only fewer active providers.
if [[ "${VERSION_ID}" == "26.04" ]]; then
  begin_compat_source_transition
  disable_installer_managed_cuda_sources ""
  disable_cuda_compat_sources "" "${CUDA_LEGACY_COMPAT_SOURCE}"
fi

apt-get update
apt-get install -y --no-install-recommends binutils ca-certificates curl zstd

keyring_deb="$(mktemp --suffix=.deb /tmp/hstream-cuda-keyring.XXXXXX)"
curl -fsSLo "${keyring_deb}" \
  "https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_REPOSITORY}/x86_64/cuda-keyring_1.1-1_all.deb"

# NVIDIA currently publishes the TensorRT 10 / CUDA 13.2 packages consumed by
# DeepStream 9.1 in its Ubuntu 24.04 repository. Resolute therefore needs that
# compatibility repository in addition to its native CUDA repository.
if [[ "${VERSION_ID}" == "26.04" ]]; then
  compat_keyring_deb="$(mktemp --suffix=.deb /tmp/hstream-cuda-compat-keyring.XXXXXX)"
  curl -fsSLo "${compat_keyring_deb}" \
    "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb"
  native_dir="$(mktemp -d /tmp/hstream-cuda-native-keyring.XXXXXX)"
  compat_dir="$(mktemp -d /tmp/hstream-cuda-keyring.XXXXXX)"
  dpkg-deb -x "${keyring_deb}" "${native_dir}"
  dpkg-deb -x "${compat_keyring_deb}" "${compat_dir}"
  for extracted_key in \
    "${native_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" \
    "${compat_dir}/usr/share/keyrings/cuda-archive-keyring.gpg"; do
    if [[ ! -s "${extracted_key}" ]]; then
      echo "ERROR: NVIDIA cuda-keyring artifact did not contain its signing key." >&2
      exit 1
    fi
  done

  # Keep the compatibility key at an installer-owned path.  Upgrading the
  # cuda-keyring package may replace its own release-specific key file, but it
  # cannot invalidate this durable two-release keyring.
  combined_keyring="$(mktemp /tmp/hstream-cuda-combined.XXXXXX.gpg)"
  cat "${native_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" \
    "${compat_dir}/usr/share/keyrings/cuda-archive-keyring.gpg" >"${combined_keyring}"
  compat_keyring_target_temp="$(mktemp /usr/share/keyrings/.hstream-cuda-compat.XXXXXX.gpg)"
  install -m 0644 "${combined_keyring}" "${compat_keyring_target_temp}"
  sync -d "${compat_keyring_target_temp}"
  mv -f "${compat_keyring_target_temp}" "${CUDA_COMPAT_KEYRING}"
  sync -f "$(dirname "${CUDA_COMPAT_KEYRING}")"
  compat_keyring_target_temp=""

  # First disable all duplicate definitions.  Each atomic file replacement
  # only removes providers, so interruption cannot split one repository across
  # conflicting Signed-By values.  Then atomically publish exactly one usable
  # amd64 binary source with the exact flat-repository suite.
  disable_cuda_compat_sources ""
  publish_cuda_compat_source ""
  commit_compat_source_transition
fi

dpkg -i "${keyring_deb}"

apt-get update
trt_runtime_version="$(apt-cache madison libnvinfer10 \
  | awk '$3 ~ /^10[.]/ && $3 ~ /[+]cuda13[.]2$/ && !found { print $3; found = 1 }')"
if [[ -z "${trt_runtime_version}" ]]; then
  echo "ERROR: NVIDIA repositories do not provide the TensorRT 10 / CUDA 13.2 dependencies required by DeepStream 9.1." >&2
  exit 1
fi

# Older HStream installers pinned every TensorRT package to version 10. That
# needlessly attempted to downgrade an independently installed TensorRT 11 SDK.
# The versioned TensorRT 10 runtime packages required by the two local .debs
# coexist with newer SDK packages and apt resolves them without a global pin.
rm -f /etc/apt/preferences.d/hstream-tensorrt10

apt_args=(-y --no-install-recommends)
if [[ "${SIMULATE}" -eq 1 ]]; then apt_args+=(--simulate); fi

# NVIDIA's versioned DeepStream artifacts install many of the same absolute
# paths but do not declare Conflicts/Replaces against older versioned releases
# (for example, deepstream-8.0).  APT does not order a package-name removal
# before unpacking a local artifact when dpkg cannot see a declared conflict.
# Add those relationships to a temporary local copy, allowing APT to perform
# one coherent replacement transaction without a standalone removal.
# Keep unrelated split packages out of this list; the 9.1 artifact declares its
# own conflicts with the legacy binaries/sample-data packages.
old_deepstream_packages=()
while IFS=$'\t' read -r package status; do
  package_name="${package%%:*}"
  if [[ "${status:0:1}" == "i" && "${status:1:1}" != "n" &&
        "${package_name}" =~ ^deepstream-[0-9]+([.][0-9]+)*$ &&
        "${package_name}" != "deepstream-9.1" ]]; then
    old_deepstream_packages+=("${package}")
  fi
done < <(dpkg-query -W -f='${binary:Package}\t${db:Status-Abbrev}\n' 'deepstream-*' 2>/dev/null || true)
install_deepstream_deb="${DEEPSTREAM_DEB}"
if [[ "${#old_deepstream_packages[@]}" -gt 0 ]]; then
  echo "Replacing older DeepStream package(s): ${old_deepstream_packages[*]}"
  transition_dir="$(mktemp -d /tmp/hstream-deepstream-transition.XXXXXX)"
  control_member="$(ar t "${DEEPSTREAM_DEB}" | awk '/^control[.]tar[.]/{print; exit}')"
  data_member="$(ar t "${DEEPSTREAM_DEB}" | awk '/^data[.]tar[.]/{print; exit}')"
  if [[ -z "${control_member}" || -z "${data_member}" ]]; then
    echo "ERROR: malformed DeepStream Debian artifact." >&2
    exit 1
  fi
  case "${control_member}" in
    *.zst) control_compression=(--zstd) ;;
    *.xz) control_compression=(-J) ;;
    *.gz) control_compression=(-z) ;;
    *) echo "ERROR: unsupported DeepStream control archive: ${control_member}" >&2; exit 1 ;;
  esac
  mkdir "${transition_dir}/control"
  ar p "${DEEPSTREAM_DEB}" "${control_member}" | tar "${control_compression[@]}" -xf - -C "${transition_dir}/control"
  transition_relationships=()
  for package in "${old_deepstream_packages[@]}"; do
    transition_relationships+=("${package%%:*}")
  done
  relationship_list="$(IFS=', '; echo "${transition_relationships[*]}")"
  sed -i -E \
    -e "s/^(Conflicts:.*)$/\\1, ${relationship_list}/" \
    -e "s/^(Replaces:.*)$/\\1, ${relationship_list}/" \
    "${transition_dir}/control/control"
  tar "${control_compression[@]}" -cf "${transition_dir}/${control_member}" -C "${transition_dir}/control" .
  install_deepstream_deb="${transition_dir}/deepstream-9.1-transition.deb"
  printf '!<arch>\n' >"${install_deepstream_deb}"
  append_ar_member() {
    local name="$1"
    local size="$2"
    printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' "${name}/" 0 0 0 100644 "${size}" >>"${install_deepstream_deb}"
  }
  for member in debian-binary "${control_member}" "${data_member}"; do
    if [[ "${member}" == "${control_member}" ]]; then
      member_size="$(stat -c '%s' "${transition_dir}/${control_member}")"
      append_ar_member "${member}" "${member_size}"
      cat "${transition_dir}/${control_member}" >>"${install_deepstream_deb}"
    else
      member_size="$(ar tv "${DEEPSTREAM_DEB}" | awk -v member="${member}" '$NF == member {print $3; exit}')"
      append_ar_member "${member}" "${member_size}"
      ar p "${DEEPSTREAM_DEB}" "${member}" >>"${install_deepstream_deb}"
    fi
    if (( member_size % 2 != 0 )); then printf '\n' >>"${install_deepstream_deb}"; fi
  done
  dpkg-deb --info "${install_deepstream_deb}" >/dev/null
  for relationship in Conflicts Replaces; do
    metadata="$(dpkg-deb -f "${install_deepstream_deb}" "${relationship}")"
    for package in "${transition_relationships[@]}"; do
      if [[ ",${metadata// /}," != *",${package},"* ]]; then
        echo "ERROR: failed to add ${relationship}: ${package} to the DeepStream transition artifact." >&2
        exit 1
      fi
    done
  done
fi

simulation="$(apt-get install --simulate --no-install-recommends "${install_deepstream_deb}" "${HSTREAM_DEB}")"
printf '%s\n' "${simulation}"
while read -r removed_package; do
  [[ -z "${removed_package}" ]] && continue
  allowed=0
  for package in "${old_deepstream_packages[@]}"; do
    if [[ "${removed_package}" == "${package%%:*}" ]]; then allowed=1; break; fi
  done
  if [[ "${allowed}" -eq 0 ]]; then
    echo "ERROR: DeepStream replacement would remove dependent package ${removed_package}; refusing." >&2
    exit 1
  fi
done < <(awk '$1 == "Remv" {print $2}' <<<"${simulation}")

if [[ "${SIMULATE}" -eq 0 ]]; then
  apt-get install "${apt_args[@]}" "${install_deepstream_deb}" "${HSTREAM_DEB}"
fi

if [[ "${SIMULATE}" -eq 1 ]]; then
  echo "Dependency resolution succeeded for Ubuntu ${VERSION_ID}."
else
  apt-get check
  echo "Installed DeepStream $(dpkg-query -W -f='${Version}' deepstream-9.1) and HStream $(dpkg-query -W -f='${Version}' hstream)."
fi
