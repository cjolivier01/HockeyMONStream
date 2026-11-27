#!/usr/bin/env bash
# Locate the newest usable DeepStream amd64 release artifact.
#
# NVIDIA publishes the amd64 DeepStream .deb only behind a Developer Program
# login -- it is absent from the CUDA apt repositories -- so the file always
# arrives by hand, wherever the browser happened to put it.  deploy.sh and
# make_deb_docker.sh both have to find it and both run from a checkout, so the
# lookup lives here rather than being written once well and once badly.
#
# NVIDIA puts the release in the package name (deepstream-9.1, deepstream-9.2)
# and the full version in the version field, so "newest" is an ordering over
# both: the release first, then the version within it.  Nothing here is pinned
# to one release; drop a newer artifact in and the build follows it.
#
# A package may also name the Ubuntu release it was built for, in
# X-Built-For-Distribution and in its filename
# (deepstream-9.2_9.2.0-1_ubuntu26.04_amd64.deb).  Such a package is built
# against that release's own GStreamer, mesa and CUDA libraries, so it is
# preferred for a target that matches and refused for one that does not.  The
# distribution is deliberately absent from Version -- dpkg compares versions
# and must not rank one Ubuntu release against another -- which is why it is
# read from the control field rather than from dpkg's version ordering.
# Untagged packages, which is everything NVIDIA publishes, keep working
# exactly as before.
#
# Source this file; it defines functions and changes no shell options.  The
# caller must have TOPDIR set to the repository root.

# The oldest release this source tree builds against.  There is no ceiling:
# a newer DeepStream is what this file exists to find.
DEEPSTREAM_MIN_VERSION="9.1.0-1"

# Results of the most recent deepstream_artifact_resolve().
DEEPSTREAM_ARTIFACT=""
# shellcheck disable=SC2034 # Read by the scripts that source this file.
DEEPSTREAM_ARTIFACT_PACKAGE=""
# shellcheck disable=SC2034 # Read by the scripts that source this file.
DEEPSTREAM_ARTIFACT_VERSION=""
# The release the selected artifact was built for (ubuntu26.04), empty when it
# does not say.  A caller relaxes an untagged package onto its target and
# leaves a matching one alone.
# shellcheck disable=SC2034 # Read by the scripts that source this file.
DEEPSTREAM_ARTIFACT_DISTRIBUTION=""

deepstream_version_supported() {
  local version="$1"
  dpkg --compare-versions "${version}" ge "${DEEPSTREAM_MIN_VERSION}"
}

# Whether a version belongs to the release its package name claims.  A
# deepstream-9.1 package carrying 9.2.0-1 would be selected on version alone
# and then rejected by the builder, whose dependency on deepstream-9.1 excludes
# 9.2: refuse the incoherent pair here instead, where it can be explained.
deepstream_version_matches_release() {
  local package="$1" version="$2"
  local bounds_name bounds_min bounds_max
  read -r bounds_name bounds_min bounds_max < <(deepstream_dependency_bounds "${package}") || return 1
  [[ -n "${bounds_name}" ]] || return 1
  dpkg --compare-versions "${version}" ge "${bounds_min}" &&
    dpkg --compare-versions "${version}" lt "${bounds_max}"
}

# The release a DeepStream package name carries, or nothing when the name is
# not one: deepstream-9.1 -> 9.1.  Package names are matched exactly so an
# unrelated deepstream-something cannot be read as a release.
deepstream_package_release() {
  local package="$1"
  [[ "${package}" =~ ^deepstream-([0-9]+(\.[0-9]+)*)$ ]] || return 1
  printf '%s' "${BASH_REMATCH[1]}"
}

# The dependency a package built against one DeepStream release must declare:
# that release's package name, at least its first version, and less than the
# next release.  deepstream-9.1 gives ">= 9.1.0-1" and "<< 9.2~", which is
# what this repository shipped when 9.1 was the only release it knew.
# Prints "NAME MIN MAX".
deepstream_dependency_bounds() {
  local package="$1"
  local release major minor
  release="$(deepstream_package_release "${package}")" || return 1
  major="${release%%.*}"
  if [[ "${release}" == *.* ]]; then
    minor="${release#*.}"
    minor="${minor%%.*}"
    printf '%s %s.0-1 %s.%s~\n' "${package}" "${release}" "${major}" "$((minor + 1))"
  else
    printf '%s %s.0-1 %s~\n' "${package}" "${release}" "$((major + 1))"
  fi
}

# The Ubuntu release a DeepStream artifact was built for (ubuntu26.04), or
# nothing when it does not say.  The control field is authoritative because it
# survives a rename; the filename is read only when the field is absent, which
# covers a package tagged by hand.  dpkg carries user-defined fields through
# untouched, so reading one costs no more than reading Package or Version.
deepstream_artifact_distribution() {
  local candidate="$1" distribution name
  distribution="$(dpkg-deb -f "${candidate}" X-Built-For-Distribution 2>/dev/null || true)"
  distribution="${distribution//[[:space:]]/}"
  if [[ -z "${distribution}" ]]; then
    name="$(basename "${candidate}")"
    if [[ "${name}" =~ _((ubuntu|debian)[0-9]+(\.[0-9]+)*)_[A-Za-z0-9][A-Za-z0-9-]*\.deb$ ]]; then
      distribution="${BASH_REMATCH[1]}"
    fi
  fi
  printf '%s' "${distribution}"
}

# Order two candidates, best first.  A package built for the target release
# wins outright: it was linked against that release's libraries, where an
# untagged package only happens to install there, and the point of publishing
# a per-release package is to stop guessing.  Between two equally native --
# or two equally untagged -- candidates the newest wins as it always has.
deepstream_candidate_preferred() {
  local native="$1" release="$2" version="$3"
  local other_native="$4" other_release="$5" other_version="$6"
  if [[ "${native}" != "${other_native}" ]]; then
    [[ "${native}" -eq 1 ]]
    return
  fi
  deepstream_identity_newer "${release}" "${version}" "${other_release}" "${other_version}"
}

# Order two DeepStream identities, newest first.  Both halves go through dpkg,
# which is the authority on Debian version ordering and already does what is
# wanted here: 9.10 outranks 9.9 rather than sorting before it, a dash
# revision outranks none, and digits inside a revision compare as numbers, so
# 9.2.3-colivier16 outranks 9.2.3-colivier9.  Returns 0 when the first
# identity is newer than the second.
deepstream_identity_newer() {
  local release="$1" version="$2" other_release="$3" other_version="$4"
  if [[ "${release}" != "${other_release}" ]]; then
    dpkg --compare-versions "${release}" gt "${other_release}"
    return
  fi
  dpkg --compare-versions "${version}" gt "${other_version}"
}

# The newest supported DeepStream installed on this machine, as "NAME VERSION",
# or nothing when there is none.  Records for another architecture cannot
# satisfy a package built here and are left out; several releases can be
# installed side by side, so the same ordering decides between them.
deepstream_installed_newest() {
  local package version status architecture release
  local selected_package="" selected_version="" selected_release=""
  local native_architecture
  native_architecture="$(dpkg --print-architecture)"

  while read -r package version status architecture; do
    [[ "${status}" == installed ]] || continue
    [[ "${architecture}" == "${native_architecture}" || "${architecture}" == all ]] || continue
    release="$(deepstream_package_release "${package}")" || continue
    deepstream_version_supported "${version}" || continue
    if [[ -z "${selected_package}" ]] ||
       deepstream_identity_newer "${release}" "${version}" "${selected_release}" "${selected_version}"; then
      selected_package="${package}"
      selected_version="${version}"
      selected_release="${release}"
    fi
  done < <(dpkg-query -W -f='${Package} ${Version} ${db:Status-Status} ${Architecture}\n' \
    'deepstream-*' 2>/dev/null || true)

  [[ -n "${selected_package}" ]] || return 1
  printf '%s %s' "${selected_package}" "${selected_version}"
}

# Whether an artifact actually carries DeepStream.  A package named for a
# release is not necessarily one: a compatibility metapackage declares the same
# name and the same dependencies but ships no /opt/nvidia/deepstream, and
# installing it in the builder would satisfy every name check and then fail at
# the first DeepStream header.  The listing is piped so it stops at the first
# match instead of decompressing a gigabyte; a metapackage answers instantly
# because there is nothing to decompress.
# Runs with pipefail off, in a subshell so the caller keeps its own setting:
# grep stops at the first match and the writers upstream die of SIGPIPE, which
# pipefail would report as a failed pipeline.  That would reject a perfectly
# good package, and only sometimes -- a small archive finishes writing before
# grep exits and is never signalled.
deepstream_artifact_has_runtime() {
  local candidate="$1"
  (
    set +o pipefail
    dpkg-deb --fsys-tarfile "${candidate}" 2>/dev/null \
      | tar -tf - 2>/dev/null \
      | grep -qm1 '^\./opt/nvidia/deepstream/'
  )
}

# Directories searched when the caller named no artifact.  Look where a manual
# download plausibly landed rather than failing on one hardcoded path.  Set
# HSTREAM_DEEPSTREAM_CACHE to add a location without editing this list.  Paths
# are kept free of ".." so the failure message can be read as the literal place
# to drop the file.  Arguments are caller-specific directories -- the package
# output root -- searched after the sibling DeepStream checkout.
deepstream_artifact_search_dirs() {
  local -a dirs=()
  if [[ -n "${HSTREAM_DEEPSTREAM_CACHE:-}" ]]; then dirs+=("${HSTREAM_DEEPSTREAM_CACHE}"); fi
  dirs+=("$(dirname "${TOPDIR}")/DeepStream/artifacts" "$@")
  if [[ -n "${HOME:-}" ]]; then dirs+=("${HOME}/Downloads" "${HOME}"); fi
  printf '%s\n' "${dirs[@]}"
}

# Resolve a usable artifact into DEEPSTREAM_ARTIFACT, with its package name and
# version in DEEPSTREAM_ARTIFACT_PACKAGE and DEEPSTREAM_ARTIFACT_VERSION.
#   $1   log prefix for the progress line ("deploy", "make_deb_docker")
#   $2   artifact the caller was given, or "" to search
#   $3   example invocation printed when nothing is found
#   $4   release being built for ("ubuntu26.04"), or "" to accept any
#   $5+  extra directories to search
# On failure, explains where it looked and returns non-zero.  A package built
# for the target release wins; otherwise the newest wins, so an old download
# left in ~/Downloads does not shadow a fresh one, and a new release does not
# need a code change to be picked up.
deepstream_artifact_resolve() {
  local log_prefix="$1" requested="$2" hint="$3" target_distribution="$4"
  shift 4
  local candidate package version release search_dir distribution native
  local selected_candidate="" selected_package="" selected_version="" selected_release=""
  local selected_distribution=""
  local -a candidates=() search_dirs=() foreign=()
  local nullglob_was_set=0

  DEEPSTREAM_ARTIFACT=""
  DEEPSTREAM_ARTIFACT_PACKAGE=""
  DEEPSTREAM_ARTIFACT_VERSION=""
  DEEPSTREAM_ARTIFACT_DISTRIBUTION=""
  if [[ -n "${requested}" ]]; then
    candidates+=("${requested}")
  else
    mapfile -t search_dirs < <(deepstream_artifact_search_dirs "$@")
    # Restore rather than clear: this is a sourced library and the setting
    # belongs to the caller.
    if shopt -q nullglob; then nullglob_was_set=1; fi
    shopt -s nullglob
    for search_dir in "${search_dirs[@]}"; do
      candidates+=("${search_dir}/"deepstream-[0-9]*_*_amd64.deb)
    done
    if [[ "${nullglob_was_set}" -eq 0 ]]; then shopt -u nullglob; fi
  fi

  # Qualify on metadata first, then take them newest first and stop at the one
  # that actually carries DeepStream.  Content is checked in that order rather
  # than up front so the usual case reads one archive, not every archive.
  local -a ranked_releases=() ranked_versions=() ranked_packages=() ranked_paths=()
  local -a ranked_distributions=() ranked_natives=()
  for candidate in "${candidates[@]}"; do
    [[ -f "${candidate}" ]] || continue
    package="$(dpkg-deb -f "${candidate}" Package 2>/dev/null || true)"
    release="$(deepstream_package_release "${package}")" || continue
    if [[ "$(dpkg-deb -f "${candidate}" Architecture 2>/dev/null || true)" != "amd64" ]]; then
      continue
    fi
    version="$(dpkg-deb -f "${candidate}" Version 2>/dev/null || true)"
    deepstream_version_supported "${version}" || continue
    deepstream_version_matches_release "${package}" "${version}" || continue
    # A package built for another release is refused rather than ranked last:
    # it was linked against that release's libraries, and installing it here
    # is the failure this tag exists to prevent.  A package that names no
    # release is accepted as it always was.
    distribution="$(deepstream_artifact_distribution "${candidate}")"
    native=0
    if [[ -n "${target_distribution}" && -n "${distribution}" ]]; then
      if [[ "${distribution}" != "${target_distribution}" ]]; then
        foreign+=("${package} ${version} built for ${distribution} (${candidate})")
        continue
      fi
      native=1
    fi
    ranked_releases+=("${release}")
    ranked_versions+=("${version}")
    ranked_packages+=("${package}")
    ranked_paths+=("${candidate}")
    ranked_distributions+=("${distribution}")
    ranked_natives+=("${native}")
  done

  if [[ "${#foreign[@]}" -gt 0 ]]; then
    printf '[%s] Passed over, built for another release than %s:\n' \
      "${log_prefix}" "${target_distribution}" >&2
    printf '  %s\n' "${foreign[@]}" >&2
  fi

  local index best
  local -a rejected=()
  while [[ "${#ranked_paths[@]}" -gt 0 ]]; do
    best=0
    for index in "${!ranked_paths[@]}"; do
      if deepstream_candidate_preferred \
        "${ranked_natives[index]}" "${ranked_releases[index]}" "${ranked_versions[index]}" \
        "${ranked_natives[best]}" "${ranked_releases[best]}" "${ranked_versions[best]}"; then
        best="${index}"
      fi
    done
    if deepstream_artifact_has_runtime "${ranked_paths[best]}"; then
      selected_candidate="${ranked_paths[best]}"
      selected_package="${ranked_packages[best]}"
      selected_version="${ranked_versions[best]}"
      selected_release="${ranked_releases[best]}"
      selected_distribution="${ranked_distributions[best]}"
      break
    fi
    rejected+=("${ranked_packages[best]} ${ranked_versions[best]} (${ranked_paths[best]})")
    unset 'ranked_releases[best]' 'ranked_versions[best]' 'ranked_packages[best]' \
      'ranked_paths[best]' 'ranked_distributions[best]' 'ranked_natives[best]'
    ranked_releases=("${ranked_releases[@]}")
    ranked_versions=("${ranked_versions[@]}")
    ranked_packages=("${ranked_packages[@]}")
    ranked_paths=("${ranked_paths[@]}")
    ranked_distributions=("${ranked_distributions[@]}")
    ranked_natives=("${ranked_natives[@]}")
  done

  if [[ "${#rejected[@]}" -gt 0 ]]; then
    printf '[%s] Passed over, carrying no /opt/nvidia/deepstream:\n' "${log_prefix}" >&2
    printf '  %s\n' "${rejected[@]}" >&2
  fi

  if [[ -n "${selected_candidate}" ]]; then
    DEEPSTREAM_ARTIFACT="$(readlink -f "${selected_candidate}")"
    # shellcheck disable=SC2034 # Read by the scripts that source this file.
    DEEPSTREAM_ARTIFACT_PACKAGE="${selected_package}"
    # shellcheck disable=SC2034 # Read by the scripts that source this file.
    DEEPSTREAM_ARTIFACT_VERSION="${selected_version}"
    # shellcheck disable=SC2034 # Read by the scripts that source this file.
    DEEPSTREAM_ARTIFACT_DISTRIBUTION="${selected_distribution}"
    # The tag is named only when there is one, so the usual untagged line
    # reads exactly as it did before.
    printf '[%s] Using %s %s%s from %s\n' \
      "${log_prefix}" "${selected_package}" "${selected_version}" \
      "${selected_distribution:+ (built for ${selected_distribution})}" \
      "${DEEPSTREAM_ARTIFACT}"
    return 0
  fi

  if [[ -n "${requested}" ]]; then
    if [[ "${#foreign[@]}" -gt 0 ]]; then
      printf 'ERROR: this DeepStream package is built for another release than %s: %s\n' \
        "${target_distribution}" "${requested}" >&2
      printf 'Point at the %s package, or at one that names no release.\n' \
        "${target_distribution}" >&2
      return 1
    fi
    printf 'ERROR: not a usable DeepStream amd64 package: %s\n' "${requested}" >&2
    printf 'Expected a deepstream-RELEASE package, architecture amd64, version >= %s,\n' \
      "${DEEPSTREAM_MIN_VERSION}" >&2
    printf 'carrying /opt/nvidia/deepstream.  A compatibility metapackage names a release\n' >&2
    printf 'and declares its dependencies but ships none of it, so it cannot be built against.\n' >&2
    return 1
  fi

  printf 'ERROR: no DeepStream amd64 artifact found%s.\n' \
    "${target_distribution:+ for ${target_distribution}}" >&2
  printf 'Searched for deepstream-*_*_amd64.deb in:\n' >&2
  for search_dir in "${search_dirs[@]}"; do
    if [[ -d "${search_dir}" ]]; then
      printf '  %s\n' "${search_dir}" >&2
    else
      printf '  %s (missing)\n' "${search_dir}" >&2
    fi
  done
  printf 'Put the package in one of those directories, or point at it directly:\n' >&2
  printf '  %s\n' "${hint}" >&2
  printf 'HSTREAM_DEEPSTREAM_CACHE=DIR adds a directory to the list above.\n' >&2
  if [[ -z "${HSTREAM_DEEPSTREAM_CACHE:-}" ]]; then
    printf 'NVIDIA gates the amd64 .deb behind a Developer Program login; it is not in\n' >&2
    printf 'the CUDA apt repositories, so it has to be downloaded by hand from\n' >&2
    printf 'https://developer.nvidia.com/deepstream-download\n' >&2
  fi
  return 1
}
