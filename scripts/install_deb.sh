#!/bin/bash
# Install the local DeepStream and HStream Debian artifacts on a clean Ubuntu
# host, including the NVIDIA repositories that provide their CUDA/TensorRT
# dependencies.  With --no-deepstream only HStream is installed, against the
# DeepStream the host already has; the repository work still runs, because
# HStream's own CUDA/TensorRT dependencies come from there.
set -euo pipefail

CUDA_COMPAT_REPOSITORY='https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/'
# Prefix shared by the release-native and the Ubuntu 24.04 compatibility CUDA
# repositories, which supply the CUDA/TensorRT dependencies of both artifacts.
CUDA_REPOSITORY_PREFIX='https://developer.download.nvidia.com/compute/cuda/repos/'
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

# APT fails the whole update when any single configured repository fails, and
# long-lived hosts accumulate third-party repositories whose subscriptions
# lapse or whose suites are withdrawn (packages.xilinx.com now answers 401 for
# noble).  APT keeps the previously fetched lists for those, so the condition
# is neither caused by this installation nor repairable from it; aborting would
# make every deployment to such a host fail for an unrelated reason.  Tolerate
# and report failures APT attributes to a repository.  Failures that name no
# repository -- lock contention, an unparsable sources file -- stay fatal, as
# does one that names a repository but describes a local fault, and as does a
# failure of any repository named in "$@", which the caller depends on for the
# packages it is about to install.
run_apt_update() {
  local -a required=("$@")
  local status tee_status diagnostics grep_status=0
  local line warning uri repository canonical prefix
  local -a stale=() required_failures=() unattributed=()
  local -A seen=()

  # The capture lives in the variable the EXIT trap clears, so that a signal
  # during the update does not leave it behind.
  apt_update_log="$(mktemp "${TMPDIR:-/tmp}/hstream-apt-update.XXXXXX.log")"
  # The diagnostics below are matched by spelling, so pin the message locale.
  local -x LC_ALL=C
  if apt-get update 2>&1 | tee "${apt_update_log}"; then
    rm -f "${apt_update_log}"
    apt_update_log=""
    return 0
  fi
  # One statement: any command in between, an assignment included, replaces
  # PIPESTATUS with its own single-element status.
  local -a pipe_status=("${PIPESTATUS[@]}")
  status="${pipe_status[0]}"
  tee_status="${pipe_status[1]:-0}"

  # The verdict is only as good as the capture.  A log cut short -- /var
  # filling as APT reports the first of several failures -- would hide a fatal
  # line behind a tolerable one, so a capture that did not complete is fatal.
  diagnostics="$(grep -E '^[EW]: ' "${apt_update_log}")" || grep_status=$?
  rm -f "${apt_update_log}"
  apt_update_log=""
  if [[ "${tee_status}" -ne 0 || "${grep_status}" -gt 1 ]]; then
    echo "ERROR: apt-get update failed with status ${status}; its output could not be recorded." >&2
    return 1
  fi

  while IFS= read -r line; do
    if [[ -z "${line}" ]]; then
      continue
    fi
    # The closing summary only restates the per-repository errors above it.
    if [[ "${line}" == 'E: Some index files failed to download.'* ]]; then
      continue
    fi
    # APT reports some fetch failures as warnings, which do not fail the
    # update.  An unrelated repository is tolerated either way, so a warning
    # matters only when it names one this installation requires -- and an
    # ordinary warning, a legacy keyring or a changed label, is not a failure
    # at all and must not be mistaken for one.
    warning=0
    if [[ "${line}" == 'W: '* ]]; then
      case "${line}" in
        'W: Failed to fetch '* | 'W: The repository '*) warning=1 ;;
        *) continue ;;
      esac
    fi
    # Both spellings carry the repository: "Failed to fetch" names an index
    # file, and "The repository '<uri> <suite> ...'" names the base.  Require a
    # transport so that a message merely containing a colon is not mistaken for
    # an attributed failure and silently downgraded to a warning.
    uri="$(grep -oE "(file:/|[a-z][a-z0-9+.-]*://)[^[:space:]'\"]+" <<<"${line}" | head -n 1 || true)"
    if [[ -z "${uri}" ]]; then
      if [[ "${warning}" -eq 0 ]]; then
        unattributed+=("${line}")
      fi
      continue
    fi
    # A fetch failure can name a URI and still describe a local fault: a full
    # disk, a read-only /var.  APT attributes those to whatever it happened to
    # be downloading, but no repository is to blame and the installation below
    # would hit the same fault, so match the cause instead of the URI.
    if [[ "${line}" == *'No space left on device'* ||
          "${line}" == *'Read-only file system'* ||
          "${line}" == *'Input/output error'* ||
          "${line}" == *'Error writing to file'* ]]; then
      if [[ "${warning}" -eq 0 ]]; then
        unattributed+=("${line}")
      fi
      continue
    fi
    repository="${uri%%/dists/*}"
    repository="${repository%/InRelease}"
    repository="${repository%/Release.gpg}"
    repository="${repository%/Release}"
    repository="${repository%/}"
    # Match the required repositories against the untruncated URI: every index
    # below a repository starts with that repository's base, while shortening
    # to the base can drop a required URI below its own prefix.  The truncated
    # spelling is what the warning should name, not what it should match.
    canonical="${uri,,}"
    canonical="${canonical/#http:/https:}"
    if [[ "${#required[@]}" -gt 0 ]]; then
      for prefix in "${required[@]}"; do
        if [[ "${canonical}" == "${prefix}"* ]]; then
          required_failures+=("${line}")
          continue 2
        fi
      done
    fi
    # Past this point the line is an unrelated repository's: a warning about
    # one is just a warning, and only a real failure makes its lists stale.
    if [[ "${warning}" -eq 1 ]]; then
      continue
    fi
    if [[ -z "${seen[${repository}]:-}" ]]; then
      seen["${repository}"]=1
      stale+=("${repository}")
    fi
  done <<<"${diagnostics}"

  if [[ "${#required_failures[@]}" -gt 0 ]]; then
    echo "ERROR: apt-get update failed for a repository this installation requires:" >&2
    printf '  %s\n' "${required_failures[@]}" >&2
    return 1
  fi
  if [[ "${#unattributed[@]}" -gt 0 ]]; then
    echo "ERROR: apt-get update failed with status ${status}:" >&2
    printf '  %s\n' "${unattributed[@]}" >&2
    return 1
  fi
  if [[ "${#stale[@]}" -eq 0 ]]; then
    echo "ERROR: apt-get update failed with status ${status}." >&2
    return 1
  fi
  echo "WARNING: apt-get update could not refresh these repositories; APT kept their previous package lists:" >&2
  printf '  %s\n' "${stale[@]}" >&2
  echo "WARNING: installation continues. Repair or remove them on this host if a package below cannot be resolved." >&2
  return 0
}

# The DeepStream packages a simulated APT transaction would change.  Leaving
# the artifact out of an installation's arguments does not stop APT from
# installing, upgrading, configuring or removing DeepStream to satisfy what it
# was given, so --no-deepstream has to judge the plan rather than the
# arguments.  Every action verb APT prints is matched, and every package whose
# name begins with deepstream: the split binaries and sample-data packages are
# as much "this host's DeepStream" as the versioned one.
planned_deepstream_changes() {
  awk '($1 == "Inst" || $1 == "Conf" || $1 == "Remv" || $1 == "Purg") && $2 ~ /^deepstream/ { print $1 " " $2 }' <<<"$1"
}

# Stop before a transaction that would break the promise, naming what it was
# going to do.  Takes a plan from a command whose exit status has already been
# checked; use refuse_deepstream_simulation otherwise.
refuse_deepstream_plan() {
  local description="$1"
  local simulation="$2"
  local planned
  planned="$(planned_deepstream_changes "${simulation}")"
  [[ -z "${planned}" ]] && return 0
  echo "ERROR: --no-deepstream, but ${description} would change DeepStream:" >&2
  printf '%s\n' "${planned}" | sed 's/^/  /' >&2
  exit 1
}

# Run a simulation and refuse the transaction if DeepStream is in its plan.
# The command runs here rather than in the caller's argument list because a
# command substitution in an argument throws its exit status away: a
# simulation that failed would arrive as a plan that changes nothing, and the
# transaction would go ahead unexamined.  An unreadable plan is not an empty
# one, so it stops the installation.
refuse_deepstream_simulation() {
  local description="$1"
  shift
  local simulation
  if ! simulation="$("$@")"; then
    echo "ERROR: --no-deepstream, but APT could not say what ${description} would do." >&2
    exit 1
  fi
  refuse_deepstream_plan "${description}" "${simulation}"
}

# Exact state of those packages, for the comparison that proves an install
# really left them alone, including through transactions not simulated above.
# A package waiting on triggers is installed and on its way to saying so, and
# the host runs its own transactions: snapshotting one mid-trigger must not
# report a successful deployment as having changed DeepStream.  Every other
# status stays distinct, so an actual removal or a broken unpack is still seen.
deepstream_package_state() {
  dpkg-query -W -f='${Package} ${Version} ${db:Status-Status} ${Architecture}\n' 'deepstream*' 2>/dev/null \
    | awk '{ if ($3 == "triggers-pending" || $3 == "triggers-awaited") $3 = "installed"; print }' \
    | sort || true
}

# Read the DeepStream release and version range an HStream package declares,
# into DEEPSTREAM_PACKAGE, DEEPSTREAM_MIN_VERSION and DEEPSTREAM_MAX_VERSION.
# Depends is a comma-separated list of relations, so split it and judge each
# one whole: matching a pattern against the field as a whole would find
# "deepstream-9.1 (>= ...)" inside "libdeepstream-9.1 (>= ...)" and would take
# the last match in the field rather than the right one.  Both relations are
# required and they have to name the same release; a package asking for
# DeepStream any other way is not one this installer knows how to satisfy.
read_deepstream_requirement() {
  local depends="$1"
  local relation name version
  DEEPSTREAM_PACKAGE=""
  DEEPSTREAM_MIN_VERSION=""
  DEEPSTREAM_MAX_VERSION=""

  # In a variable: bash tokenizes << and >= inside [[ ]] before it ever gets
  # to the match.
  local relation_pattern='^(deepstream-[0-9]+(\.[0-9]+)*)[[:space:]]*\((>=|<<)[[:space:]]*([^)[:space:]]+)[[:space:]]*\)$'
  local saved_ifs="${IFS}"
  IFS=','
  # Deliberately unquoted: this is the split.
  # shellcheck disable=SC2086
  set -- ${depends}
  IFS="${saved_ifs}"
  for relation in "$@"; do
    relation="${relation#"${relation%%[![:space:]]*}"}"
    relation="${relation%"${relation##*[![:space:]]}"}"
    [[ "${relation}" =~ ${relation_pattern} ]] || continue
    name="${BASH_REMATCH[1]}"
    version="${BASH_REMATCH[4]}"
    if [[ -n "${DEEPSTREAM_PACKAGE}" && "${name}" != "${DEEPSTREAM_PACKAGE}" ]]; then
      DEEPSTREAM_PACKAGE=""
      return 1
    fi
    DEEPSTREAM_PACKAGE="${name}"
    if [[ "${BASH_REMATCH[3]}" == ">=" ]]; then
      DEEPSTREAM_MIN_VERSION="${version}"
    else
      DEEPSTREAM_MAX_VERSION="${version}"
    fi
  done

  [[ -n "${DEEPSTREAM_PACKAGE}" && -n "${DEEPSTREAM_MIN_VERSION}" && -n "${DEEPSTREAM_MAX_VERSION}" ]]
}

# Is APT removing a package this installation is supposed to remove?
#   removal_is_expected REMOVED EXPECTED...
# APT names a foreign-architecture package "deepstream-9.1:arm64" and the
# native one "deepstream-9.1", while the names it is checked against are bare.
removal_is_expected() {
  local removed="$1"
  local expected
  shift
  for expected in "$@"; do
    if [[ "${removed}" == "${expected}" || "${removed}" == "${expected}:"* ]]; then
      return 0
    fi
  done
  return 1
}

# The package names a Debian artifact declares a conflict with, one per line.
# NVIDIA's DeepStream artifacts conflict with the legacy split packages, so
# APT removes whichever of them a host still has.  That is the artifact's own
# instruction rather than an unexpected dependent, and reading it out of the
# artifact keeps the architecture-specific name (deepstream-binaries-x86 or
# -aarch64) out of this script.
declared_conflicts() {
  # A relation carrying a version, "pkg (<< 9.2)", says the conflict applies
  # to some versions and not others.  Approving the name alone would also
  # approve removing a version the artifact never objected to, so a versioned
  # relation is not reported and the removal guard refuses it: the artifacts
  # declare these unversioned, and a visible refusal beats a silent removal.
  dpkg-deb -f "$1" Conflicts | tr ',' '\n' | awk 'NF && NF == 1 { print $1 }'
}

# Name the installed DeepStream releases a new one is about to take shared
# paths from, reading dpkg-query '${binary:Package}\t${db:Status-Abbrev}' lines
# on stdin:
#   dpkg-query ... | superseded_deepstream_packages KEEP
#
# What matters is whether a release still owns files, which is the current
# status, not the desired one: a held release reads "hi" and one an
# interrupted apt already marked for removal reads "ri", and both still own
# every path dpkg recorded for them.  Only "n" (gone) and "c" (config files
# only) own none.  Split packages such as deepstream-sample-data are left out;
# the artifacts declare their own conflicts with those.
superseded_deepstream_packages() {
  local keep="$1"
  local package status package_name
  while IFS=$'\t' read -r package status; do
    package_name="${package%%:*}"
    if [[ "${status:1:1}" != "n" && "${status:1:1}" != "c" &&
          "${package_name}" =~ ^deepstream-[0-9]+([.][0-9]+)*$ &&
          "${package_name}" != "${keep}" ]]; then
      printf '%s\n' "${package_name}"
    fi
  done
}

# Write a copy of a DeepStream artifact that declares Conflicts and Replaces
# against the older DeepStream package names it is about to take the shared
# paths from, and prove the copy carries them:
#   build_deepstream_transition_deb SOURCE_DEB OUTPUT_DEB PACKAGE...
#
# The output's directory also holds this function's scratch while it works,
# and OUTPUT_DEB itself is replaced, so give it one the caller owns.
#
# The fields are added when the paragraph does not already have one.  NVIDIA's
# artifacts carry Conflicts but no Replaces at all, so a rewrite that can only
# extend an existing line leaves out the relationship that lets dpkg hand those
# paths over, which is the reason for the copy.
build_deepstream_transition_deb() {
  local source_deb="$1"
  local output_deb="$2"
  shift 2
  local -a relationships=("$@")
  local output_dir work_dir control_dir control_member data_member relationship_list partial_deb
  local relationship package member member_size header_end written_size metadata
  local -a control_compression=()

  # Without a package to name, the verification below has nothing to look for
  # and would pass an artifact carrying two empty fields.
  if [[ "${#relationships[@]}" -eq 0 ]]; then
    echo "ERROR: no DeepStream release to declare the transition against." >&2
    return 1
  fi

  control_member="$(ar t "${source_deb}" | awk '/^control[.]tar([.]|$)/{print; exit}')"
  data_member="$(ar t "${source_deb}" | awk '/^data[.]tar([.]|$)/{print; exit}')"
  if [[ -z "${control_member}" || -z "${data_member}" ]]; then
    echo "ERROR: malformed DeepStream Debian artifact." >&2
    return 1
  fi
  case "${control_member}" in
    *.zst) control_compression=(--zstd) ;;
    *.xz) control_compression=(-J) ;;
    *.gz) control_compression=(-z) ;;
    control.tar) control_compression=() ;;
    *) echo "ERROR: unsupported DeepStream control archive: ${control_member}" >&2; return 1 ;;
  esac

  output_dir="$(dirname "${output_deb}")"
  # Assembled under another name and renamed once every check below has
  # passed, so the path the caller hands APT either does not exist or is a
  # complete artifact - never the part of one that a failure got to write.
  partial_deb="${output_deb}.partial"
  rm -f "${output_deb}" "${partial_deb}"
  # Scratch beside the output, under a name mktemp just created: this removes
  # it again at the end, and a directory that was already there is not ours
  # to delete.
  work_dir="$(mktemp -d "${output_dir}/transition.XXXXXX")"
  control_dir="${work_dir}/control"
  mkdir "${control_dir}"
  ar p "${source_deb}" "${control_member}" \
    | tar "${control_compression[@]}" -xf - -C "${control_dir}"

  relationship_list="$(printf ', %s' "${relationships[@]}")"
  relationship_list="${relationship_list#, }"
  # Edit the first paragraph only, and edit it as Deb822 rather than as lines.
  # A relationship can be folded across continuation lines, so the list joins
  # the field's last line.  A missing field goes in before the blank line that
  # ends the paragraph; after one, it would start a second paragraph with no
  # Package and dpkg would refuse the whole control file.  Blank lines before
  # the first field do not end anything, because the paragraph has not begun.
  # Field names are matched without case and tolerate the space dpkg allows
  # before the colon, since a second Replaces beside a "replaces" or a
  # "Replaces " is equally unparsable.
  awk -v list="${relationship_list}" '
    # dpkg reads a control file with CRLF endings, so the list goes in front
    # of the carriage return rather than after it, where it would land in the
    # middle of the value.
    function body_of(line) {
      sub(/\r$/, "", line)
      return line
    }
    function append_to(line, text) {
      if (line ~ /\r$/) { return substr(line, 1, length(line) - 1) text "\r" }
      return line text
    }
    function field_at(line,   slot, name, head) {
      if (match(line, /^[^ \t:]+[ \t]*:/) == 0) { return "" }
      head = substr(line, 1, RLENGTH - 1)
      sub(/[ \t]*$/, "", head)
      for (slot = 1; slot <= wanted_count; slot++) {
        name = wanted[slot]
        if (toupper(head) == toupper(name)) { return name }
      }
      return ""
    }
    BEGIN { wanted_count = split("Conflicts Replaces", wanted, " ") }
    !started && /^\r?$/ { leading[++leading_count] = $0; next }
    !started { started = 1; in_paragraph = 1 }
    in_paragraph && /^\r?$/ { in_paragraph = 0 }
    in_paragraph {
      paragraph[++paragraph_count] = $0
      if ($0 ~ /^[ \t]/) {
        if (current != "") {
          last_line[current] = paragraph_count
          if (body_of($0) ~ /[^ \t]/) { has_value[current] = 1 }
        }
      } else {
        current = field_at($0)
        if (current != "") {
          last_line[current] = paragraph_count
          if (substr(body_of($0), index($0, ":") + 1) ~ /[^ \t]/) { has_value[current] = 1 }
        }
      }
      next
    }
    { rest[++rest_count] = $0 }
    END {
      for (slot = 1; slot <= wanted_count; slot++) {
        name = wanted[slot]
        if (name in last_line) {
          # dpkg accepts an empty relationship field when another one follows
          # it.  A comma in front of the list would make that field a
          # relationship list with a hole in it, which dpkg does not accept.
          if (name in has_value) {
            paragraph[last_line[name]] = append_to(paragraph[last_line[name]], ", " list)
          } else {
            paragraph[last_line[name]] = append_to(paragraph[last_line[name]], " " list)
          }
        } else {
          paragraph[++paragraph_count] = name ": " list
        }
      }
      for (line = 1; line <= leading_count; line++) { print leading[line] }
      for (line = 1; line <= paragraph_count; line++) { print paragraph[line] }
      for (line = 1; line <= rest_count; line++) { print rest[line] }
    }
  ' "${control_dir}/control" >"${work_dir}/control.rewritten"
  # Written outside the extracted tree: a control member carrying its own
  # "control.rewritten" symlink would otherwise aim this redirection, which
  # runs as root, at whatever the artifact chose.
  mv -f "${work_dir}/control.rewritten" "${control_dir}/control"
  tar "${control_compression[@]}" -cf "${work_dir}/${control_member}" -C "${control_dir}" .

  # Assemble the members directly.  Rebuilding with dpkg-deb would recompress
  # a gigabyte that did not change, and "cp" plus "ar r" would hold two copies
  # of it at once - ar writes its temporary archive in the working directory,
  # which is not even a path this script chose.  Streaming the data member
  # through keeps one copy, on a filesystem the caller picked.
  printf '!<arch>\n' >"${partial_deb}"
  append_ar_member() {
    local name="$1"
    local size="$2"
    printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' "${name}/" 0 0 0 100644 "${size}" >>"${partial_deb}"
  }
  for member in debian-binary "${control_member}" "${data_member}"; do
    if [[ "${member}" == "${control_member}" ]]; then
      member_size="$(stat -c '%s' "${work_dir}/${control_member}")"
      append_ar_member "${member}" "${member_size}"
      header_end="$(stat -c '%s' "${partial_deb}")"
      cat "${work_dir}/${control_member}" >>"${partial_deb}"
    else
      member_size="$(ar tv "${source_deb}" | awk -v member="${member}" '$NF == member {print $3; exit}')"
      append_ar_member "${member}" "${member_size}"
      header_end="$(stat -c '%s' "${partial_deb}")"
      ar p "${source_deb}" "${member}" >>"${partial_deb}"
    fi
    # The header above promises a length.  Measure what actually landed:
    # every reader downstream, dpkg-deb --info included, trusts the header
    # and would hand APT a package dpkg only rejects at unpack, after it has
    # removed the release being replaced.
    written_size="$(( $(stat -c '%s' "${partial_deb}") - header_end ))"
    if [[ "${written_size}" != "${member_size}" ]]; then
      echo "ERROR: wrote ${written_size} of ${member_size} bytes for ${member} in the DeepStream transition artifact." >&2
      return 1
    fi
    if (( member_size % 2 != 0 )); then printf '\n' >>"${partial_deb}"; fi
  done
  rm -rf "${work_dir:?}"

  # dpkg-deb reads the control member, so this proves the archive it was
  # written into is one dpkg can walk.
  dpkg-deb --info "${partial_deb}" >/dev/null
  for relationship in Conflicts Replaces; do
    metadata="$(dpkg-deb -f "${partial_deb}" "${relationship}")"
    for package in "${relationships[@]}"; do
      if [[ ",${metadata// /}," != *",${package},"* ]]; then
        echo "ERROR: failed to add ${relationship}: ${package} to the DeepStream transition artifact." >&2
        return 1
      fi
    done
  done
  mv -f "${partial_deb}" "${output_deb}"
}

# Allow the behavior test to source the production implementation without
# entering the privileged installer workflow.
if [[ "${HSTREAM_INSTALLER_SOURCE_ONLY:-0}" == "1" ]]; then
  # shellcheck disable=SC2317 # exit is used when executed rather than sourced.
  return 0 2>/dev/null || exit 0
fi

HSTREAM_DEB=""
DEEPSTREAM_DEB=""
NO_DEEPSTREAM=0
# Which DeepStream release, and which versions of it, are read out of the
# HStream package's own Depends field below.  Nothing here names a release.
DEEPSTREAM_PACKAGE=""
DEEPSTREAM_MIN_VERSION=""
DEEPSTREAM_MAX_VERSION=""
SIMULATE=0
FORCE_HSTREAM=0

usage() {
  cat <<'USAGE'
Usage:
  sudo ./install-hstream-deb \
    --deepstream-deb=/path/to/deepstream-9.1_9.1.1-1_amd64.deb \
    --hstream-deb=/path/to/hstream_*_amd64.deb

  sudo ./install-hstream-deb --no-deepstream \
    --hstream-deb=/path/to/hstream_*_amd64.deb

Options:
  --deepstream-deb FILE  The DeepStream release artifact this HStream package
                         was built against, named in its Depends field.
  --hstream-deb FILE    Local HStream artifact for this Ubuntu release.
  --no-deepstream        Install HStream alone against the DeepStream already
                         on this host, which is left exactly as it is.  The
                         NVIDIA repositories are still configured, because
                         HStream's own CUDA and TensorRT dependencies come
                         from them.
  --force-hstream       Replace HStream even when this version is installed;
                        also permit an explicit downgrade to the local version.
  --simulate             Configure repositories and only simulate apt install.
  -h, --help             Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deepstream-deb) DEEPSTREAM_DEB="$2"; shift ;;
    --deepstream-deb=*) DEEPSTREAM_DEB="${1#*=}" ;;
    --no-deepstream) NO_DEEPSTREAM=1 ;;
    --hstream-deb) HSTREAM_DEB="$2"; shift ;;
    --hstream-deb=*) HSTREAM_DEB="${1#*=}" ;;
    --force-hstream) FORCE_HSTREAM=1 ;;
    --simulate) SIMULATE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

# Judge the request before the privilege check: a caller who asked for the
# wrong thing should be told that, not told to come back as root first.
if [[ "${NO_DEEPSTREAM}" -eq 1 && -n "${DEEPSTREAM_DEB}" ]]; then
  echo "ERROR: --no-deepstream and --deepstream-deb request opposite things." >&2
  usage >&2
  exit 1
fi
if [[ -z "${HSTREAM_DEB}" ]] || [[ -z "${DEEPSTREAM_DEB}" && "${NO_DEEPSTREAM}" -eq 0 ]]; then
  echo "ERROR: --hstream-deb and --deepstream-deb are required." >&2
  echo "Pass --no-deepstream instead to keep the DeepStream already installed here." >&2
  usage >&2
  exit 1
fi
if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: run this installer as root (for example, with sudo)." >&2
  exit 1
fi

LOCAL_DEBS=("${HSTREAM_DEB}")
if [[ "${NO_DEEPSTREAM}" -eq 0 ]]; then LOCAL_DEBS+=("${DEEPSTREAM_DEB}"); fi
for deb in "${LOCAL_DEBS[@]}"; do
  if [[ ! -f "${deb}" ]]; then
    echo "ERROR: Debian artifact not found: ${deb}" >&2
    exit 1
  fi
done
HSTREAM_DEB="$(readlink -f "${HSTREAM_DEB}")"
if [[ "$(dpkg-deb -f "${HSTREAM_DEB}" Package)" != "hstream" ]]; then
  echo "ERROR: not an hstream package: ${HSTREAM_DEB}" >&2
  exit 1
fi
# The package states which DeepStream it was built against, so read it from
# there rather than carrying a release this installer would have to be edited
# to follow.
HSTREAM_DEPENDS="$(dpkg-deb -f "${HSTREAM_DEB}" Depends 2>/dev/null || true)"
read_deepstream_requirement "${HSTREAM_DEPENDS}"
if [[ -z "${DEEPSTREAM_PACKAGE}" || -z "${DEEPSTREAM_MIN_VERSION}" || -z "${DEEPSTREAM_MAX_VERSION}" ]]; then
  echo "ERROR: ${HSTREAM_DEB} does not declare which DeepStream release it needs." >&2
  echo "Depends: ${HSTREAM_DEPENDS:-(none)}" >&2
  exit 1
fi

if [[ "${NO_DEEPSTREAM}" -eq 1 ]]; then
  # Nothing here will install DeepStream, so say so now rather than letting APT
  # explain it as an unmet dependency after the repository work below.  A
  # multiarch host can hold several records for this name -- a foreign one left
  # in config-files, say -- so select the record that can actually satisfy the
  # package instead of reading whatever dpkg-query prints first.
  installed_deepstream_version="$(dpkg-query -W \
    -f='${db:Status-Status} ${Architecture} ${Version}\n' "${DEEPSTREAM_PACKAGE}" 2>/dev/null \
    | awk -v arch="$(dpkg --print-architecture)" \
      '$1 == "installed" && ($2 == arch || $2 == "all") { print $3; exit }' || true)"
  if [[ -z "${installed_deepstream_version}" ]] ||
     ! dpkg --compare-versions "${installed_deepstream_version}" ge "${DEEPSTREAM_MIN_VERSION}" ||
     ! dpkg --compare-versions "${installed_deepstream_version}" lt "${DEEPSTREAM_MAX_VERSION}"; then
    echo "ERROR: --no-deepstream needs ${DEEPSTREAM_PACKAGE} >= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION} installed here." >&2
    echo "Installed version: ${installed_deepstream_version:-not installed}" >&2
    echo "Rerun with --deepstream-deb=FILE to install it." >&2
    exit 1
  fi
else
  DEEPSTREAM_DEB="$(readlink -f "${DEEPSTREAM_DEB}")"
  if [[ "$(dpkg-deb -f "${DEEPSTREAM_DEB}" Package)" != "${DEEPSTREAM_PACKAGE}" ]]; then
    echo "ERROR: ${HSTREAM_DEB} needs ${DEEPSTREAM_PACKAGE}, but this is $(dpkg-deb -f "${DEEPSTREAM_DEB}" Package): ${DEEPSTREAM_DEB}" >&2
    exit 1
  fi
  deepstream_version="$(dpkg-deb -f "${DEEPSTREAM_DEB}" Version)"
  if ! dpkg --compare-versions "${deepstream_version}" ge "${DEEPSTREAM_MIN_VERSION}" ||
     ! dpkg --compare-versions "${deepstream_version}" lt "${DEEPSTREAM_MAX_VERSION}"; then
    echo "ERROR: ${DEEPSTREAM_PACKAGE} >= ${DEEPSTREAM_MIN_VERSION}, << ${DEEPSTREAM_MAX_VERSION} is required: ${DEEPSTREAM_DEB}" >&2
    echo "Found version: ${deepstream_version}" >&2
    exit 1
  fi
fi

if [[ ! -r /etc/os-release ]]; then
  echo "ERROR: cannot identify the target operating system." >&2
  exit 1
fi
# shellcheck disable=SC1091
source /etc/os-release
# Ubuntu derivatives such as KDE neon and Pop!_OS report their own ID, and some
# (Linux Mint, Zorin) also report a VERSION_ID unrelated to any Ubuntu release.
# UBUNTU_CODENAME names the Ubuntu base they are assembled from, so resolve the
# release through it and accept only the codenames HStream publishes for.
UBUNTU_RELEASE=""
UBUNTU_BASE_CODENAME=""
if [[ "${ID:-}" == "ubuntu" ]]; then
  UBUNTU_RELEASE="${VERSION_ID:-}"
elif [[ " ${ID_LIKE:-} " == *" ubuntu "* ]]; then
  UBUNTU_BASE_CODENAME="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
  # Keep this table in sync with ubuntu_release() in scripts/deploy.sh.  Bases
  # HStream does not publish for are still resolved so the failure below names
  # the actual release instead of claiming the host is not a derivative.
  case "${UBUNTU_BASE_CODENAME}" in
    jammy) UBUNTU_RELEASE=22.04 ;;
    noble) UBUNTU_RELEASE=24.04 ;;
    resolute) UBUNTU_RELEASE=26.04 ;;
  esac
fi
if [[ -z "${UBUNTU_RELEASE}" ]]; then
  if [[ " ${ID_LIKE:-} " == *" ubuntu "* ]]; then
    echo "ERROR: cannot map ${PRETTY_NAME:-${ID:-unknown}} onto a known Ubuntu base." >&2
    echo "UBUNTU_CODENAME is ${UBUNTU_BASE_CODENAME:-unset}; expected jammy, noble or resolute." >&2
  else
    echo "ERROR: HStream Debian artifacts require Ubuntu or an Ubuntu derivative." >&2
    echo "Detected: ${PRETTY_NAME:-${ID:-unknown} ${VERSION_ID:-unknown}}" >&2
  fi
  exit 1
fi
case "${UBUNTU_RELEASE}" in
  24.04) CUDA_REPOSITORY=ubuntu2404 ;;
  26.04) CUDA_REPOSITORY=ubuntu2604 ;;
  *)
    echo "ERROR: unsupported Ubuntu release: ${UBUNTU_RELEASE} (expected 24.04 or 26.04)." >&2
    exit 1
    ;;
esac
if [[ "${ID:-}" != "ubuntu" ]]; then
  echo "Treating ${PRETTY_NAME:-${ID:-unknown}} as its Ubuntu ${UBUNTU_RELEASE} base."
fi

HSTREAM_TARGET_UBUNTU="$(dpkg-deb -f "${HSTREAM_DEB}" X-HStream-Target-Ubuntu 2>/dev/null || true)"
if [[ "${HSTREAM_TARGET_UBUNTU}" != "${UBUNTU_RELEASE}" ]]; then
  echo "ERROR: the selected HStream artifact targets Ubuntu ${HSTREAM_TARGET_UBUNTU:-unknown}, not ${UBUNTU_RELEASE}." >&2
  exit 1
fi

HOST_ARCH="$(dpkg --print-architecture)"
LOCAL_DEBS=("${HSTREAM_DEB}")
if [[ "${NO_DEEPSTREAM}" -eq 0 ]]; then LOCAL_DEBS+=("${DEEPSTREAM_DEB}"); fi
for deb in "${LOCAL_DEBS[@]}"; do
  deb_arch="$(dpkg-deb -f "${deb}" Architecture)"
  if [[ "${deb_arch}" != "${HOST_ARCH}" ]]; then
    echo "ERROR: ${deb} targets ${deb_arch}, but this host is ${HOST_ARCH}." >&2
    exit 1
  fi
done

export DEBIAN_FRONTEND=noninteractive
apt_update_log=""
keyring_deb=""
compat_keyring_deb=""
native_dir=""
compat_dir=""
relaxed_deepstream_dir=""
combined_keyring=""
compat_keyring_target_temp=""
transition_dir=""
cleanup() {
  local status=$?
  set +e
  if [[ "${status}" -ne 0 && "${compat_source_transition_committed}" -eq 0 ]]; then
    restore_compat_source_transition
  fi
  if [[ -n "${apt_update_log}" ]]; then rm -f "${apt_update_log}"; fi
  if [[ -n "${keyring_deb}" ]]; then rm -f "${keyring_deb}"; fi
  if [[ -n "${compat_keyring_deb}" ]]; then rm -f "${compat_keyring_deb}"; fi
  if [[ -n "${native_dir}" ]]; then rm -rf "${native_dir}"; fi
  if [[ -n "${compat_dir}" ]]; then rm -rf "${compat_dir}"; fi
  if [[ -n "${relaxed_deepstream_dir}" ]]; then rm -rf "${relaxed_deepstream_dir}"; fi
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
if [[ "${UBUNTU_RELEASE}" == "26.04" ]]; then
  begin_compat_source_transition
  disable_installer_managed_cuda_sources ""
  disable_cuda_compat_sources "" "${CUDA_LEGACY_COMPAT_SOURCE}"
fi

# The CUDA repositories are not required yet: an existing NVIDIA source whose
# key is missing is exactly what installing cuda-keyring below repairs.  A
# distribution archive that could not be refreshed is reported and the install
# continues on whatever lists APT already had, which is the point of tolerating
# it: the next line fails if those lists cannot supply the bootstrap packages,
# and succeeds if they can.
run_apt_update

# Record the state --no-deepstream promises to preserve before the first APT
# transaction, not just before the last one: any transaction can configure a
# DeepStream component this host left half-installed.
DEEPSTREAM_STATE_BEFORE=""
if [[ "${NO_DEEPSTREAM}" -eq 1 ]]; then
  DEEPSTREAM_STATE_BEFORE="$(deepstream_package_state)"
  refuse_deepstream_simulation "installing the bootstrap packages" \
    apt-get install --simulate -y --no-install-recommends binutils ca-certificates curl zstd
fi
apt-get install -y --no-install-recommends binutils ca-certificates curl zstd

# Keep this in sync with scripts/remove_deb_dependencies.py, which relaxes the
# same artifact at package-build time.  Two pins block a 24.04 DeepStream deb
# on 26.04: version relations carrying a 24.04 tag, and dependencies naming a
# specific CUDA minor toolkit.  NVIDIA carries only a couple of minors per
# release, so "cuda-cudart-13-0 | cuda-cudart-13-2" resolves nowhere on a host
# whose CUDA repository publishes 13.1, 13.3 and 13.4.  Every CUDA 13 component
# also provides a minor-independent virtual package, which is the one stable
# way to depend on the major alone.  The virtual name is not derivable from the
# binary name -- libcufft-13-3 provides libcufft.so.12 -- so it is a table.
relax_deepstream_dependency_pins() {
  local input_deb="$1"
  local output_deb="$2"
  local package_root
  package_root="$(dirname "${output_deb}")/package"
  mkdir -p "${package_root}"
  dpkg-deb -R "${input_deb}" "${package_root}"
  if [[ ! -f "${package_root}/DEBIAN/control" ]]; then
    echo "ERROR: DeepStream Debian artifact has no control file." >&2
    exit 1
  fi
  awk '
    BEGIN {
      cuda_virtual["cuda-cudart"] = "libcudart.so.13"
      cuda_virtual["cuda-cudart-dev"] = "libcudart.so.13-dev"
      cuda_virtual["cuda-cupti"] = "libcupti.so.13"
      cuda_virtual["cuda-cupti-dev"] = "libcupti.so.13-dev"
      cuda_virtual["cuda-driver-dev"] = "libcuda.so.13-dev"
      cuda_virtual["cuda-nvrtc"] = "libnvrtc.so.13"
      cuda_virtual["cuda-nvrtc-dev"] = "libnvrtc.so.13-dev"
      cuda_virtual["cuda-opencl"] = "libopencl.so.1"
      cuda_virtual["cuda-opencl-dev"] = "libopencl.so.1-dev"
      cuda_virtual["libcublas"] = "libcublas.so.13"
      cuda_virtual["libcublas-dev"] = "libcublas.so.13-dev"
      cuda_virtual["libcufft"] = "libcufft.so.12"
      cuda_virtual["libcufft-dev"] = "libcufft.so.12-dev"
      cuda_virtual["libcufile-dev"] = "libcufile.so.0-dev"
      cuda_virtual["libcuobjclient-dev"] = "libcuobjclient.so.0-dev"
      cuda_virtual["libcurand"] = "libcurand.so.10"
      cuda_virtual["libcurand-dev"] = "libcurand.so.10-dev"
      cuda_virtual["libcusolver"] = "libcusolver.so.12"
      cuda_virtual["libcusolver-dev"] = "libcusolver.so.12-dev"
      cuda_virtual["libcusparse"] = "libcusparse.so.12"
      cuda_virtual["libcusparse-dev"] = "libcusparse.so.12-dev"
      cuda_virtual["libnpp"] = "libnpp.so.13"
      cuda_virtual["libnpp-dev"] = "libnpp.so.13-dev"
      cuda_virtual["libnvfatbin"] = "libnvfatbin.so.13"
      cuda_virtual["libnvfatbin-dev"] = "libnvfatbin.so.13-dev"
      cuda_virtual["libnvjitlink"] = "libnvjitlink.so.13"
      cuda_virtual["libnvjitlink-dev"] = "libnvjitlink.so.13-dev"
      cuda_virtual["libnvjpeg"] = "libnvjpeg.so.13"
      cuda_virtual["libnvjpeg-dev"] = "libnvjpeg.so.13-dev"
      field_name = ""
      field_text = ""
    }
    function relax_alternative(alternative,   name, rest, arch, colon, base) {
      gsub(/^[ \t]+|[ \t]+$/, "", alternative)
      if (!match(alternative, /^[^ \t(]+/))
        return alternative
      name = substr(alternative, 1, RLENGTH)
      rest = substr(alternative, RLENGTH + 1)
      colon = index(name, ":")
      arch = ""
      if (colon > 0) {
        arch = substr(name, colon)
        name = substr(name, 1, colon - 1)
      }
      if (match(name, /-13-[0-9]+$/)) {
        base = substr(name, 1, RSTART - 1)
        if (base in cuda_virtual)
          name = cuda_virtual[base]
      }
      alternative = name arch rest
      # Mirrors DEFAULT_VERSION_REGEX: the 24.04 has to start the version or
      # follow a separator, so 1.24.04x and 124.04 are left alone.
      sub(/[ \t]*\((<<|<=|=|>=|>>)[ \t]*(([^)]*[~+.:_-])?24[.]04([~+.:_-][^)]*)?|[^)]*ubuntu[0-9]*[~+.:_-]?24[.]04[^)]*)\)/, "", alternative)
      gsub(/^[ \t]+|[ \t]+$/, "", alternative)
      return alternative
    }
    function relax_value(value,   groups, group_count, i, alternatives, alternative_count,
                         j, alternative, kept, kept_count, seen, out) {
      group_count = split(value, groups, /,/)
      out = ""
      for (i = 1; i <= group_count; i++) {
        alternative_count = split(groups[i], alternatives, /\|/)
        kept = ""
        kept_count = 0
        delete seen
        for (j = 1; j <= alternative_count; j++) {
          alternative = relax_alternative(alternatives[j])
          # Two minor pins of one component collapse onto the same virtual
          # package, so the alternative would otherwise be repeated.
          if (alternative == "" || (alternative in seen))
            continue
          seen[alternative] = 1
          kept = (kept_count++ ? kept " | " : "") alternative
        }
        if (kept != "")
          out = (out == "" ? "" : out ", ") kept
      }
      return out
    }
    function emit(  value) {
      if (field_text == "")
        return
      if (field_name ~ /^(Depends|Pre-Depends|Recommends|Suggests)$/) {
        value = field_text
        sub(/^[^:]*:/, "", value)
        gsub(/\n[ \t]*/, " ", value)
        gsub(/^[ \t]+|[ \t]+$/, "", value)
        printf "%s: %s\n", field_name, relax_value(value)
      } else {
        printf "%s", field_text
      }
      field_name = ""
      field_text = ""
    }
    /^[ \t]/ {
      if (field_text != "") {
        field_text = field_text $0 "\n"
        next
      }
    }
    {
      emit()
      field_text = $0 "\n"
      field_name = ""
      if ($0 ~ /^[^ \t][^:]*:/) {
        field_name = $0
        sub(/:.*/, "", field_name)
      }
    }
    END { emit() }
  ' "${package_root}/DEBIAN/control" >"${package_root}/DEBIAN/control.relaxed"
  mv -f "${package_root}/DEBIAN/control.relaxed" "${package_root}/DEBIAN/control"
  dpkg-deb --build --root-owner-group "${package_root}" "${output_deb}" >/dev/null
}

if [[ "${UBUNTU_RELEASE}" == "26.04" && "${NO_DEEPSTREAM}" -eq 0 ]]; then
  relaxed_deepstream_dir="$(mktemp -d /tmp/hstream-deepstream-relaxed.XXXXXX)"
  relaxed_deepstream_deb="${relaxed_deepstream_dir}/${DEEPSTREAM_PACKAGE}-ubuntu26-relaxed.deb"
  echo "Relaxing Ubuntu 24.04 and CUDA minor-toolkit DeepStream dependency pins for Ubuntu 26.04..."
  relax_deepstream_dependency_pins "${DEEPSTREAM_DEB}" "${relaxed_deepstream_deb}"
  DEEPSTREAM_DEB="${relaxed_deepstream_deb}"
fi

keyring_deb="$(mktemp --suffix=.deb /tmp/hstream-cuda-keyring.XXXXXX)"
curl -fsSLo "${keyring_deb}" \
  "https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_REPOSITORY}/x86_64/cuda-keyring_1.1-1_all.deb"

# NVIDIA currently publishes the TensorRT 10 / CUDA 13.2 packages consumed by
# DeepStream 9.1 in its Ubuntu 24.04 repository. Resolute therefore needs that
# compatibility repository in addition to its native CUDA repository.
if [[ "${UBUNTU_RELEASE}" == "26.04" ]]; then
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

run_apt_update "${CUDA_REPOSITORY_PREFIX}"
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
if [[ "${FORCE_HSTREAM}" -eq 1 ]]; then apt_args+=(--allow-downgrades); fi
installed_hstream_version="$(dpkg-query -W -f='${Version}' hstream 2>/dev/null || true)"
requested_hstream_version="$(dpkg-deb -f "${HSTREAM_DEB}" Version)"

# NVIDIA's versioned DeepStream artifacts install many of the same absolute
# paths but do not declare Conflicts/Replaces against older versioned releases
# (for example, deepstream-8.0).  APT does not order a package-name removal
# before unpacking a local artifact when dpkg cannot see a declared conflict.
# Add those relationships to a temporary local copy, allowing APT to perform
# one coherent replacement transaction without a standalone removal.
# Keep unrelated split packages out of this list; the 9.1 artifact declares its
# own conflicts with the legacy binaries/sample-data packages.
old_deepstream_packages=()
if [[ "${NO_DEEPSTREAM}" -eq 0 ]]; then
  # Read the inventory in this shell rather than through a pipeline, so a
  # broken dpkg database stops here.  Deciding the transition off a query
  # that failed would hand APT an artifact with no Replaces, and APT removes
  # the old release before dpkg discovers it cannot unpack the new one.
  # Exit 1 is the wildcard matching nothing, which is a clean host.
  deepstream_inventory=""
  deepstream_inventory_status=0
  deepstream_inventory="$(dpkg-query -W -f='${binary:Package}\t${db:Status-Abbrev}\n' \
    'deepstream-*' 2>/dev/null)" || deepstream_inventory_status=$?
  if [[ "${deepstream_inventory_status}" -gt 1 ]]; then
    echo "ERROR: cannot read the installed DeepStream packages (dpkg-query exited ${deepstream_inventory_status})." >&2
    exit 1
  fi
  while IFS= read -r package_name; do
    old_deepstream_packages+=("${package_name}")
  done < <(printf '%s\n' "${deepstream_inventory}" \
    | superseded_deepstream_packages "${DEEPSTREAM_PACKAGE}")
fi
install_deepstream_deb="${DEEPSTREAM_DEB}"
if [[ "${#old_deepstream_packages[@]}" -gt 0 ]]; then
  echo "Replacing older DeepStream package(s): ${old_deepstream_packages[*]}"
  transition_dir="$(mktemp -d /tmp/hstream-deepstream-transition.XXXXXX)"
  install_deepstream_deb="${transition_dir}/${DEEPSTREAM_PACKAGE}-transition.deb"
  build_deepstream_transition_deb "${DEEPSTREAM_DEB}" "${install_deepstream_deb}" \
    "${old_deepstream_packages[@]}"
fi

install_debs=()
if [[ "${NO_DEEPSTREAM}" -eq 0 ]]; then install_debs+=("${install_deepstream_deb}"); fi
install_debs+=("${HSTREAM_DEB}")

simulation="$(apt-get install --simulate --no-install-recommends "${install_debs[@]}")"
printf '%s\n' "${simulation}"
# hmstream is the package HStream replaced; APT removing it is this
# installation's own doing, and it is matched the same way as the rest so an
# architecture-qualified "hmstream:arm64" does not abort the migration.
expected_removals=(hmstream "${old_deepstream_packages[@]}")
if [[ "${NO_DEEPSTREAM}" -eq 0 ]]; then
  while IFS= read -r conflicting_package; do
    expected_removals+=("${conflicting_package}")
  done < <(declared_conflicts "${DEEPSTREAM_DEB}")
fi
while read -r removed_package; do
  [[ -z "${removed_package}" ]] && continue
  allowed=0
  if removal_is_expected "${removed_package}" "${expected_removals[@]}"; then allowed=1; fi
  if [[ "${allowed}" -eq 0 ]]; then
    echo "ERROR: this installation would remove dependent package ${removed_package}; refusing." >&2
    exit 1
  fi
  # A host with APT::Get::Purge set turns these removals into purges, so the
  # guard has to read both verbs or it judges nothing at all on that host.
done < <(awk '$1 == "Remv" || $1 == "Purg" {print $2}' <<<"${simulation}")

if [[ "${NO_DEEPSTREAM}" -eq 1 ]]; then
  # The assignment above already failed the installation if APT could not
  # produce this plan, so the plan itself is all that is left to judge.
  refuse_deepstream_plan "installing HStream" "${simulation}"
fi

if [[ "${SIMULATE}" -eq 0 ]]; then
  apt-get install "${apt_args[@]}" "${install_debs[@]}"
  if [[ "${FORCE_HSTREAM}" -eq 1 && "${installed_hstream_version}" == "${requested_hstream_version}" ]]; then
    # A second transaction, and --reinstall was not part of the plan judged
    # above, so judge this one too rather than discovering it afterwards.
    if [[ "${NO_DEEPSTREAM}" -eq 1 ]]; then
      refuse_deepstream_simulation "reinstalling the same HStream version" \
        apt-get install --simulate -y --no-install-recommends --reinstall "${HSTREAM_DEB}"
    fi
    apt-get install -y --no-install-recommends --reinstall "${HSTREAM_DEB}"
  fi
fi

if [[ "${SIMULATE}" -eq 1 ]]; then
  echo "Dependency resolution succeeded for Ubuntu ${UBUNTU_RELEASE}."
else
  apt-get check
  # Prove the promise instead of asserting it: every transaction above was
  # judged before it ran, and this is what the host actually looks like now.
  if [[ "${NO_DEEPSTREAM}" -eq 1 ]]; then
    deepstream_state_after="$(deepstream_package_state)"
    if [[ "${deepstream_state_after}" != "${DEEPSTREAM_STATE_BEFORE}" ]]; then
      echo "ERROR: --no-deepstream, but this host's DeepStream packages changed during installation." >&2
      echo "Before:" >&2
      printf '%s\n' "${DEEPSTREAM_STATE_BEFORE:-  (none installed)}" | sed 's/^/  /' >&2
      echo "After:" >&2
      printf '%s\n' "${deepstream_state_after:-  (none installed)}" | sed 's/^/  /' >&2
      exit 1
    fi
    echo "Installed HStream $(dpkg-query -W -f='${Version}' hstream) against this host's DeepStream ${installed_deepstream_version}, which was not modified."
  else
    echo "Installed ${DEEPSTREAM_PACKAGE} $(dpkg-query -W -f='${Version}' "${DEEPSTREAM_PACKAGE}") and HStream $(dpkg-query -W -f='${Version}' hstream)."
  fi
fi
