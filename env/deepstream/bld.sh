#!/bin/bash
set -e

REAL_SCRIPT_DIR=$(dirname $(realpath "${BASH_SOURCE}"))
SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")

source "${REAL_SCRIPT_DIR}/../tools.sh"

DOCKER_OPTS=""

DOCKER_TAG=$(get_tag)
if [ ! -z "${DOCKER_TAG}" ]; then
  DOCKER_OPTS="-t ${DOCKER_TAG}"
  echo "DOCKER_TAG=${DOCKER_TAG}"
fi

docker build --memory 45G  --build-arg USERNAME=$(whoami) --build-arg UID=$(id -u) --build-arg GID=$(id -g)  ${DOCKER_OPTS} $@ .

MAJOR_VERSION="0"
NEW_VERSION="${MAJOR_VERSION}.$(increment_tag_minor_version)"
NEW_TAG="${USER}/$(get_tag):${NEW_VERSION}"
echo "Tagging to ${NEW_TAG}"
docker tag ${DOCKER_TAG} ${NEW_TAG}
