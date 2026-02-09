#!/bin/bash

REAL_SCRIPT_DIR=$(dirname $(realpath "${BASH_SOURCE}"))
SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")

source "${REAL_SCRIPT_DIR}/../tools.sh"

DOCKER_OPTS=""

if [ -z "${DOCKER_TAG}" ]; then
  DOCKER_TAG=$(get_tag)
fi

if [ ! -z "${DOCKER_TAG}" ]; then
  DOCKER_OPTS="${DOCKER_TAG}"
  echo "DOCKER_TAG=${DOCKER_TAG}"
fi

#   -v /etc/sudoers:/etc/sudoers:ro 

# Local zfs pool dir (if any) in case some symlinks go there (large storage pool)
LOCAL_POOL=""
if [ -d "/${USER}-pool" ]; then
  LOCAL_POOL="-v /${USER}-pool:/${USER}-pool"
fi

docker exec --privileged --user=$(id -u):$(id -g) -it \
  -e DEEPSTREAM_CONTAINER=1 \
  -e DISPLAY=${DISPLAY} \
  -e DISPLAY=$DISPLAY -e CUDA_CACHE_DISABLE=0 \
  --memory 32g \
  -v /mnt:/mnt \
  ${LOCAL_POOL} \
  -v ${HOME}:${HOME} \
  -v ${HOME}/.ssh:${HOME}/.ssh \
  -v /opt:/host_opt:rw \
  -v /etc/passwd:/etc/passwd:ro \
  -v /etc/group:/etc/group:ro \
  -v /etc/shadow:/etc/shadow:ro \
  -v /etc/gshadow:/etc/gshadow:ro \
  --workdir=${HOME} ${DOCKER_OPTS} $@

