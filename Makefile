BUILD_DIR ?= build
CMAKE_BUILD_TYPE ?= RelWithDebInfo
PLUGIN_SO := $(BUILD_DIR)/fluidglass.so
GIT_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
DEV_STAMP := $(shell date -u +%Y%m%dT%H%M%SZ)
DEV_PLUGIN_SO := $(BUILD_DIR)/fluidglass-dev-$(DEV_STAMP)-$(GIT_COMMIT).so

.PHONY: all configure build artifact print-artifact dev-artifact clean

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)"

build: configure
	cmake --build "$(BUILD_DIR)" --target fluidglass

artifact: build
	@printf '%s\n' "$(abspath $(PLUGIN_SO))"

print-artifact: artifact

dev-artifact: build
	cp "$(PLUGIN_SO)" "$(DEV_PLUGIN_SO)"
	@printf '%s\n' "$(abspath $(DEV_PLUGIN_SO))"

clean:
	cmake --build "$(BUILD_DIR)" --target clean
