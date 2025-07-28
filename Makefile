.DEFAULT_GOAL := build
CONTAINER_NAME = edk2-rk3588:builder
SHELL := /bin/bash

setup:
	docker build -t ${CONTAINER_NAME} .
	git submodule update --init --recursive

build: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5a -r RELEASE"

build-debug: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5a -r DEBUG"

shell: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME}
