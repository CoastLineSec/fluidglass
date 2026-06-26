cmake_minimum_required(VERSION 3.16)
project(fluidglass VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
# Hyprland headers (ABI-locked to the running Hyprland). hyprpm provides these
# for the user's exact version; a manual build uses the system hyprland package.
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

# nlohmann/json is vendored as a self-contained single header (include/), so the
# only build dependency is the Hyprland headers above — no system json package.
set(FLUIDGLASS_JSON_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Build metadata baked into the binary for diagnostics.
string(TIMESTAMP FLUIDGLASS_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE FLUIDGLASS_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT FLUIDGLASS_GIT_COMMIT)
    set(FLUIDGLASS_GIT_COMMIT "unknown")
endif()
set(FLUIDGLASS_BUILD_ID "${FLUIDGLASS_BUILD_TIME}-${FLUIDGLASS_GIT_COMMIT}")

add_library(fluidglass SHARED
    src/main.cpp
)

target_include_directories(fluidglass PRIVATE ${FLUIDGLASS_JSON_INCLUDE})
target_link_libraries(fluidglass PRIVATE PkgConfig::HYPRLAND)
target_compile_options(fluidglass PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(fluidglass PRIVATE
    FLUIDGLASS_PLUGIN_VERSION="${PROJECT_VERSION}"
    FLUIDGLASS_BUILD_ID="${FLUIDGLASS_BUILD_ID}"
    FLUIDGLASS_BUILD_TIME="${FLUIDGLASS_BUILD_TIME}"
    FLUIDGLASS_GIT_COMMIT="${FLUIDGLASS_GIT_COMMIT}"
    FLUIDGLASS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
set_target_properties(fluidglass PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS fluidglass LIBRARY DESTINATION lib/hyprland)
