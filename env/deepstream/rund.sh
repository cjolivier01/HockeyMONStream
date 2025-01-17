#!/bin/bash

REAL_SCRIPT_DIR=$(dirname $(realpath "${BASH_SOURCE}"))
SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")

source "${REAL_SCRIPT_DIR}/../tools.sh"

DOCKER_OPTS=""

DOCKER_TAG=$(get_tag)
if [ ! -z "${DOCKER_TAG}" ]; then
  DOCKER_OPTS="${DOCKER_TAG}"
  echo "DOCKER_TAG=${DOCKER_TAG}"
fi

LOCAL_POOL=""
if [ -d "/${USER}-pool" ]; then
  LOCAL_POOL="-v /${USER}-pool:/${USER}-pool"
fi

docker run ${GPU_FLAGS} --privileged --user=$(id -u):$(id -g) -it \
  -e DEEPSTREAM_CONTAINER=1 \
  -e DISPLAY=${DISPLAY} \
  --rm -v /tmp/.X11-unix:/tmp/.X11-unix -e DISPLAY=$DISPLAY -e CUDA_CACHE_DISABLE=0 \
  --memory 32g \
  -p 22298:22298 \
  --runtime nvidia \
  -v /mnt:/mnt \
  ${LOCAL_POOL} \
  -v ${HOME}:${HOME} \
  -v ${HOME}/.ssh:${HOME}/.ssh \
  -v /etc/passwd:/etc/passwd:ro \
  -v /etc/group:/etc/group:ro \
  -v /etc/shadow:/etc/shadow:ro \
  -v /etc/gshadow:/etc/gshadow:ro \
  --workdir=${HOME} $@ ${DOCKER_OPTS}
