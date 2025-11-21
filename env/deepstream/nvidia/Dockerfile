FROM nvcr.io/nvidia/cuda:11.8.0-devel-ubuntu18.04
# FROM nvcr.io/nvidia/cuda:11.2.2-cudnn8-runtime-ubuntu16.04

ENV DEBIAN_FRONTEND=noninteractive


ARG USERNAME
ARG UID
ARG GID

ENV SHELL=/bin/bash

USER root
COPY ./sdkmanager_2.2.0-12021_amd64.deb /
RUN apt-get update
RUN apt-get upgrade -y
RUN apt-get install -y sudo vim 
RUN apt install -y /sdkmanager_2.2.0-12021_amd64.deb
RUN apt-get upgrade -y
RUN apt install -y openssh-client usbutils
RUN apt install -y udev
# RUN apt install -y update-binfmts qemu-user-static

RUN echo '%sudo   ALL=(ALL:ALL) ALL' >> /etc/sudoers
RUN echo "$USERNAME ALL=(ALL:ALL) ALL" >> /etc/sudoers

ENV SHELL=/bin/bash


