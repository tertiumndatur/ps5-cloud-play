SHELL := /bin/bash
.DEFAULT_GOAL := app

HOST_CXX ?= $(shell command -v clang++-18 >/dev/null 2>&1 && echo clang++-18 || echo clang++)
OPENGL_SDK := .deps/ps5-opengl/current
APP_DEFINITIONS := CLOUDPLAY_PS5=1 SDL_MAIN_HANDLED SDL_STATIC_LIB GL_GLEXT_PROTOTYPES=1
APP_INCLUDE_PATHS := $(OPENGL_SDK)/include include src third_party third_party/chiaki-cloud/lib/include build/chiaki-ps5/lib/include build/chiaki-ps5/lib/protobuf
APP_STATIC_ARCHIVES := build/chiaki-ps5/lib/libchiaki.a build/json-c-ps5/libjson-c.a build/chiaki-ps5/third-party/nanopb/libprotobuf-nanopb.a build/chiaki-ps5/third-party/libjerasure.a build/chiaki-ps5/third-party/libgf_complete.a build/chiaki-ps5/_deps/miniupnpc-build/libminiupnpc.a build/obj/libemutls.a .deps/ps5-opengl/libps5opengl-group.a
APP_IMPORT_STUBS := $(OPENGL_SDK)/lib/libSceAgc.so $(OPENGL_SDK)/lib/libSceAgcDriver.so
PACBREW_PACKAGES := sdl2 libcurl json-c libevent openssl opus libjpeg
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS PACBREW_PACKAGES

.PHONY: app ffpkg ffpfsc release-packages libc chiaki json-c emutls opengl doctor deps test allocator-test deploy undeploy clean

doctor:
	@bash tools/doctor.sh

deps:
	@bash tools/setup-native-dependencies.sh
	@bash tools/prepare-opengl.sh

opengl:
	@bash tools/prepare-opengl.sh

build/runtime-shim/libc.prx:
	@bash tools/rebuild-libc.sh

libc: build/runtime-shim/libc.prx

chiaki:
	@bash tools/build-chiaki-ps5.sh

json-c:
	@bash tools/build-json-c-ps5.sh

emutls:
	@bash tools/build-emutls.sh

app: build/runtime-shim/libc.prx chiaki json-c emutls opengl
	@bash tools/build.sh Folder

ffpkg: build/runtime-shim/libc.prx chiaki json-c emutls opengl
	@bash tools/build.sh Ffpkg

ffpfsc: build/runtime-shim/libc.prx chiaki json-c emutls opengl
	@bash tools/build.sh Ffpfsc

release-packages: build/runtime-shim/libc.prx chiaki json-c emutls opengl
	@bash tools/build.sh All

test:
	@mkdir -p build/tests
	@$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Werror -Iinclude -Ithird_party \
		src/credentials.cpp src/cloud_settings.cpp src/cloud_access_setup.cpp src/credential_http.cpp \
		src/cloud_catalog.cpp src/recent_launches.cpp src/storage_io.cpp src/app_log.cpp \
		tests/test_cloudplay.cpp -o build/tests/test_cloudplay
	@build/tests/test_cloudplay

allocator-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror -pthread \
		src/ps5_allocator.c third_party/tlsf/tlsf.c tests/test_ps5_allocator.c \
		-o build/tests/test_ps5_allocator
	@build/tests/test_ps5_allocator

deploy:
	@printf '%s\n' '==> [deploy] Building and publishing the selected app output over FTP'
	@bash tools/deploy.sh

undeploy:
	@printf '%s\n' '==> [undeploy] Removing staged development files for this title over FTP'
	@bash tools/deploy.sh undeploy

clean:
	@rm -rf build dist
