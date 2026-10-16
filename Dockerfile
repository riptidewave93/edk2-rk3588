FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    git \
    gcc \
    g++ \
    build-essential \
    gcc-aarch64-linux-gnu \
    acpica-tools \
    python3-pyelftools \
    python3-cryptography \
    uuid-dev \
    python-is-python3 \
    device-tree-compiler \
    curl \
    && apt-get clean \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory /repo \
    && git config --global --add safe.directory /repo/edk2 \
    && git config --global --add safe.directory /repo/arm-trusted-firmware \
    && git config --global --add safe.directory /repo/devicetree/mainline/upstream \
    && git config --global --add safe.directory /repo/optee_os

VOLUME [ "/repo" ]
