.DEFAULT_GOAL := build
CONTAINER_NAME = edk2-rk3588:builder
SHELL := /bin/bash

setup:
	docker build -t ${CONTAINER_NAME} .
	git submodule update --init --recursive

build: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5a -r RELEASE --secure-boot"
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5bplus -r RELEASE --secure-boot"

build-debug: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5a -r DEBUG --secure-boot"
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME} bash -c "cd /repo && ./build.sh -d rock-5bplus -r DEBUG --secure-boot"

shell: setup
	docker run -it --rm -v ./:/repo ${CONTAINER_NAME}

clean:
	rm -rf ./*_NOR_FLASH.img
	rm -rf ./*_RAWEDK2.img
