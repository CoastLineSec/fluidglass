cmake_minimum_required(VERSION 3.16)
project(hyprfluidglass VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
# Hyprland headers (ABI-locked to the running Hyprland). hyprpm provides these
# for the user's exact version; a manual build uses the system hyprland package.
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

# nlohmann/json is vendored as a self-contained single header (include/), so the
# only build dependency is the Hyprland headers above — no system json package.
set(HYPRFLUIDGLASS_JSON_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Build metadata baked into the binary for diagnostics.
string(TIMESTAMP HYPRFLUIDGLASS_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE HYPRFLUIDGLASS_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HYPRFLUIDGLASS_GIT_COMMIT)
    set(HYPRFLUIDGLASS_GIT_COMMIT "unknown")
endif()
set(HYPRFLUIDGLASS_BUILD_ID "${HYPRFLUIDGLASS_BUILD_TIME}-${HYPRFLUIDGLASS_GIT_COMMIT}")

add_library(hyprfluidglass SHARED
    src/main.cpp
)

target_include_directories(hyprfluidglass PRIVATE ${HYPRFLUIDGLASS_JSON_INCLUDE})
target_link_libraries(hyprfluidglass PRIVATE PkgConfig::HYPRLAND)
target_compile_options(hyprfluidglass PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-gnu-unique>
)
target_compile_definitions(hyprfluidglass PRIVATE
    HYPRFLUIDGLASS_PLUGIN_VERSION="${PROJECT_VERSION}"
    HYPRFLUIDGLASS_BUILD_ID="${HYPRFLUIDGLASS_BUILD_ID}"
    HYPRFLUIDGLASS_BUILD_TIME="${HYPRFLUIDGLASS_BUILD_TIME}"
    HYPRFLUIDGLASS_GIT_COMMIT="${HYPRFLUIDGLASS_GIT_COMMIT}"
    HYPRFLUIDGLASS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
set_target_properties(hyprfluidglass PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS hyprfluidglass LIBRARY DESTINATION lib/hyprland)

include(CTest)
if(BUILD_TESTING)
    add_library(hyprfluidglass-v2-core STATIC
        src/v2/model/Material.cpp
        src/v2/model/Target.cpp
    )
    target_include_directories(hyprfluidglass-v2-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(hyprfluidglass-v2-core PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(hyprfluidglass-core-tests tests/core_tests.cpp)
    target_include_directories(hyprfluidglass-core-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_options(hyprfluidglass-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-core COMMAND hyprfluidglass-core-tests)

    add_executable(hyprfluidglass-material-tests tests/material_tests.cpp)
    target_include_directories(hyprfluidglass-material-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-material-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-material-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-material COMMAND hyprfluidglass-material-tests)

    add_executable(hyprfluidglass-target-tests tests/target_tests.cpp)
    target_include_directories(hyprfluidglass-target-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-target-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-target-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-target COMMAND hyprfluidglass-target-tests)
endif()
