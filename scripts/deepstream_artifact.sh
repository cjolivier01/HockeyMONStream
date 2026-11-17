#!/usr/bin/env bash
# Locate a DeepStream 9.1 amd64 release artifact.
#
# NVIDIA publishes the amd64 DeepStream .deb only behind a Developer Program
# login -- it is absent from the CUDA apt repositories -- so the file always
# arrives by hand, wherever the browser happened to put it.  deploy.sh and
# make_deb_docker.sh both have to find it and both run from a checkout, so the
# lookup lives here rather than being written once well and once badly.
#
# Source this file; it defines functions and changes no shell options.  The
# caller must have TOPDIR set to the repository root.

DEEPSTREAM_MIN_VERSION="9.1.0-1"
DEEPSTREAM_MAX_VERSION="9.2~"

# Result of the most recent deepstream_artifact_resolve().
DEEPSTREAM_ARTIFACT=""

deepstream_version_supported() {
  local version="$1"
  dpkg --compare-versions "${version}" ge "${DEEPSTREAM_MIN_VERSION}" &&
    dpkg --compare-versions "${version}" lt "${DEEPSTREAM_MAX_VERSION}"
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

# Resolve a usable artifact into DEEPSTREAM_ARTIFACT.
#   $1   log prefix for the progress line ("deploy", "make_deb_docker")
#   $2   artifact the caller was given, or "" to search
#   $3   example invocation printed when nothing is found
#   $4+  extra directories to search
# On failure, explains where it looked and returns non-zero.  The newest
# supported artifact wins, so an old download left in ~/Downloads does not
# shadow a fresh one.
deepstream_artifact_resolve() {
  local log_prefix="$1" requested="$2" hint="$3"
  shift 3
  local candidate version selected_candidate="" selected_version="" search_dir
  local -a candidates=() search_dirs=()
  local nullglob_was_set=0

  DEEPSTREAM_ARTIFACT=""
  if [[ -n "${requested}" ]]; then
    candidates+=("${requested}")
  else
    mapfile -t search_dirs < <(deepstream_artifact_search_dirs "$@")
    # Restore rather than clear: this is a sourced library and the setting
    # belongs to the caller.
    if shopt -q nullglob; then nullglob_was_set=1; fi
    shopt -s nullglob
    for search_dir in "${search_dirs[@]}"; do
      candidates+=("${search_dir}/"deepstream-9.1_*_amd64.deb)
    done
    if [[ "${nullglob_was_set}" -eq 0 ]]; then shopt -u nullglob; fi
  fi

  for candidate in "${candidates[@]}"; do
    [[ -f "${candidate}" ]] || continue
    if [[ "$(dpkg-deb -f "${candidate}" Package 2>/dev/null || true)" != "deepstream-9.1" ||
          "$(dpkg-deb -f "${candidate}" Architecture 2>/dev/null || true)" != "amd64" ]]; then
      continue
    fi
    version="$(dpkg-deb -f "${candidate}" Version 2>/dev/null || true)"
    if deepstream_version_supported "${version}"; then
      if [[ -z "${selected_version}" ]] || dpkg --compare-versions "${version}" gt "${selected_version}"; then
        selected_candidate="${candidate}"
        selected_version="${version}"
      fi
    fi
  done

  if [[ -n "${selected_candidate}" ]]; then
    DEEPSTREAM_ARTIFACT="$(readlink -f "${selected_candidate}")"
    printf '[%s] Using DeepStream %s from %s\n' \
      "${log_prefix}" "${selected_version}" "${DEEPSTREAM_ARTIFACT}"
    return 0
  fi

  if [[ -n "${requested}" ]]; then
    printf 'ERROR: not a supported DeepStream 9.1 amd64 package: %s\n' "${requested}" >&2
    printf 'Expected package deepstream-9.1, architecture amd64, version >= %s and << %s.\n' \
      "${DEEPSTREAM_MIN_VERSION}" "${DEEPSTREAM_MAX_VERSION}" >&2
    return 1
  fi

  printf 'ERROR: no DeepStream 9.1 amd64 artifact found.\n' >&2
  printf 'Searched for deepstream-9.1_*_amd64.deb in:\n' >&2
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
