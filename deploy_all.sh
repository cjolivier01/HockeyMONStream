#!/bin/bash
set -euo pipefail

# Deploy to the nodes named by HSTREAM_DEPLOY_NODES, for example:
#
#   export HSTREAM_DEPLOY_NODES='monster,mini,meshy,stubby JETSON_QT=1'
#   ./deploy_all.sh
#
# The first word is the comma-separated node list, spaces not allowed inside
# it. The words after it must be make variable assignments such as JETSON_QT=1,
# and are handed over unparsed, so a quote in one stays literal and
# JETSON_QT="1" would reach deploy.sh with the quotes still attached.
#
# Arguments to this script follow those and are not inspected: they reach make
# as they would if you ran it yourself, so that is where a value with a space
# belongs, and where NODES= can still be overridden for one run.
#
#   ./deploy_all.sh 'DEPLOY_OUTPUT_DIR=/tmp/a b'

SCRIPT_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")"

nodes=""
extra=()
if [ -n "${HSTREAM_DEPLOY_NODES:-}" ]; then
  # Split on whitespace, newlines and carriage returns included: a value edited
  # on Windows otherwise ends up with an invisible CR inside the node name.
  # -f keeps a name such as 'mini*' from globbing against the working directory.
  set -f
  saved_ifs="${IFS}"
  IFS=$' \t\n\r'
  # shellcheck disable=SC2206 # deliberate word splitting
  fields=(${HSTREAM_DEPLOY_NODES})
  IFS="${saved_ifs}"
  set +f
  if [ "${#fields[@]}" -gt 0 ]; then
    nodes="${fields[0]}"
    extra=("${fields[@]:1}")
  fi
fi

if [ -n "${nodes}" ]; then
  # scripts/deploy.sh checks node names, but make and the recipe shell rewrite
  # NODES before it looks: 'mini$1' and 'mini`true`' both arrive there as mini,
  # so a bad name can turn into a real and different machine. This variable is
  # usually exported once and forgotten, so check it here and leave what makes
  # a node valid to deploy.sh.
  if [[ ! "${nodes}" =~ ^[A-Za-z0-9_.@,-]+$ ]]; then
    echo "HSTREAM_DEPLOY_NODES: '${nodes}' is not a plain comma-separated node list." >&2
    echo "Node names hold letters, digits, dot, dash, underscore and an optional user@." >&2
    exit 2
  fi
  # make's assignment operators, minus :::=, which make 4.3 on Ubuntu 24.04
  # does not have. Operator soup such as JETSON_QT++=1 is not one of them
  # either: that assigns a variable named JETSON_QT+ and leaves JETSON_QT at
  # its default, so the deploy would quietly drop the setting. The name is
  # held to [A-Za-z_][A-Za-z0-9_]*, which is narrower than make allows and
  # covers every variable this Makefile reads.
  assign_op='(=|:=|::=|\+=|\?=|!=)'
  for word in "${extra[@]}"; do
    if [[ "${word}" =~ ^NODES${assign_op} ]]; then
      # Depending on the operator this can send the deploy somewhere other than
      # the first word says, so every form of it is refused.
      echo "HSTREAM_DEPLOY_NODES: '${word}' is a second NODES assignment." >&2
      echo "The node list is the first word; it belongs there and nowhere else here." >&2
      exit 2
    fi
    if [[ ! "${word}" =~ ^[A-Za-z_][A-Za-z0-9_]*${assign_op} ]]; then
      # Usually this is the node list written with a space in it. The stray
      # piece becomes a make goal, and make runs deploy before failing on an
      # unknown one, so 'monster, mini' would deploy to monster alone.
      echo "HSTREAM_DEPLOY_NODES: '${word}' is not a VAR=VALUE assignment." >&2
      echo "Write the node list as one word without spaces, then settings like JETSON_QT=1." >&2
      exit 2
    fi
  done
  exec make -C "${SCRIPT_DIR}" deploy NODES="${nodes}" "${extra[@]}" "$@"
fi

echo "HSTREAM_DEPLOY_NODES is unset or empty; running 'make deploy' without a node list." >&2
echo "Set it to deploy to several machines at once:" >&2
echo "  HSTREAM_DEPLOY_NODES='monster,mini,stubby JETSON_QT=1'" >&2
exec make -C "${SCRIPT_DIR}" deploy "$@"
