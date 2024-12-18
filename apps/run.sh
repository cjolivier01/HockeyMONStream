#!/bin/bash
SCRIPT_DIR=$(dirname $(realpath $0))

APP_FILE="$1"
APP_NAME="$(basename ${APP_FILE} .yaml)"
#RUNDIR="/tmp"
RUNDIR="${SCRIPT_DIR}/../../rundir"
MANIFEST_FILE="${RUNDIR}/ds.${APP_NAME}/manifest.yaml"

echo "MANIFEST_FILE=${MANIFEST_FILE}"

#if [ ! -f "${MANIFEST_FILE}" ]; then
RUN_POSTFIX="-run=false"  execute_graph.sh  --fresh-manifest "${APP_FILE}" 
#fi

GRAPH_FILES="${APP_FILE}" \
  gxe \
  -app "${APP_FILE}" \
  -manifest "${MANIFEST_FILE}" \
  -severity=4 \
  $@

