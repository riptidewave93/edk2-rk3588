FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    git \
    gcc \
    g++ \
    build-essential \
    gcc-aarch64-linux-gnu \
    acpica-tools \
    python3-pyelftools \
    uuid-dev \
    python-is-python3 \
    device-tree-compiler \
    curl \
    && apt-get clean \
    && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory /repo

VOLUME [ "/repo" ]
