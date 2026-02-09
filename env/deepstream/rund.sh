#!/bin/bash

REAL_SCRIPT_DIR=$(dirname $(realpath "${BASH_SOURCE}"))
SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")

source "${REAL_SCRIPT_DIR}/../tools.sh"

DOCKER_OPTS=""

if [ -z "${DOCKER_TAG}" ]; then
  DOCKER_TAG=$(get_tag)
  if [ ! -z "${DOCKER_TAG}" ]; then
    DOCKER_OPTS="${DOCKER_TAG}"
    echo "DOCKER_TAG=${DOCKER_TAG}"
  fi
fi

LOCAL_POOL=""
POOL_USER_NAME=olivier
if [ -d "/${POOL_USER_NAME}-pool" ]; then
  LOCAL_POOL="-v /${POOL_USER_NAME}-pool:/${POOL_USER_NAME}-pool"
fi

#   -v /etc/group:/etc/group:rw
#  -v /etc/shadow:/etc/shadow:rw
#  -v /etc/gshadow:/etc/gshadow:rw
# 
# colivier ALL=(ALL:ALL) ALL
docker run ${GPU_FLAGS} --privileged --user=$(id -u):$(id -g) -it \
  -e DEEPSTREAM_CONTAINER=1 \
  -e DISPLAY=${DISPLAY} \
  --rm -v /tmp/.X11-unix:/tmp/.X11-unix -e DISPLAY=$DISPLAY -e CUDA_CACHE_DISABLE=0 \
  --memory 32g \
  -p 22298:22298 \
  --runtime nvidia \
  -v /mnt:/mnt \
   -v /dev/bus/usb:/dev/bus/usb/ -v /dev:/dev -v /media/$USER:/media/nvidia:slave \
  --network host \
  -dit \
  ${LOCAL_POOL} \
  -v ${HOME}:${HOME} \
  -v ${HOME}/.ssh:${HOME}/.ssh \
  -v /etc/passwd:/etc/passwd:rw \
  --workdir=${HOME} $@ ${DOCKER_OPTS}
