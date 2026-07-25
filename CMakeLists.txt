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
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)

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

add_library(hyprfluidglass-v2-core STATIC
    src/v2/core/OpaqueId.cpp
    src/v2/ipc/Protocol.cpp
    src/v2/model/Config.cpp
    src/v2/model/Material.cpp
    src/v2/model/Readiness.cpp
    src/v2/model/Session.cpp
    src/v2/model/Target.cpp
    src/v2/runtime/Runtime.cpp
)
target_include_directories(hyprfluidglass-v2-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${HYPRFLUIDGLASS_JSON_INCLUDE}"
)
target_compile_options(hyprfluidglass-v2-core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-gnu-unique>
)
target_link_libraries(hyprfluidglass PRIVATE hyprfluidglass-v2-core)

add_library(hyprfluidglass-v2-lua STATIC
    src/v2/config/LuaConfig.cpp
)
target_include_directories(hyprfluidglass-v2-lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hyprfluidglass-v2-lua PUBLIC
    hyprfluidglass-v2-core
    PkgConfig::LUA
)
target_compile_options(hyprfluidglass-v2-lua PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-gnu-unique>
)

include(CTest)
if(BUILD_TESTING)
    add_executable(hyprfluidglass-core-tests tests/core_tests.cpp)
    target_include_directories(hyprfluidglass-core-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_options(hyprfluidglass-core-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-core COMMAND hyprfluidglass-core-tests)

    add_executable(hyprfluidglass-opaque-id-tests tests/opaque_id_tests.cpp)
    target_include_directories(hyprfluidglass-opaque-id-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-opaque-id-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-opaque-id-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-opaque-id COMMAND hyprfluidglass-opaque-id-tests)

    add_executable(hyprfluidglass-lua-config-tests tests/lua_config_tests.cpp)
    target_include_directories(hyprfluidglass-lua-config-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-lua-config-tests PRIVATE hyprfluidglass-v2-lua)
    target_compile_options(hyprfluidglass-lua-config-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-lua-config COMMAND hyprfluidglass-lua-config-tests)

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

    add_executable(hyprfluidglass-session-tests tests/session_tests.cpp)
    target_include_directories(hyprfluidglass-session-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-session-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-session-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-session COMMAND hyprfluidglass-session-tests)

    add_executable(hyprfluidglass-config-tests tests/config_tests.cpp)
    target_include_directories(hyprfluidglass-config-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-config-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-config-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-config COMMAND hyprfluidglass-config-tests)

    add_executable(hyprfluidglass-readiness-tests tests/readiness_tests.cpp)
    target_include_directories(hyprfluidglass-readiness-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-readiness-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-readiness-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-readiness COMMAND hyprfluidglass-readiness-tests)

    add_executable(hyprfluidglass-protocol-tests tests/protocol_tests.cpp)
    target_include_directories(hyprfluidglass-protocol-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${HYPRFLUIDGLASS_JSON_INCLUDE}"
    )
    target_link_libraries(hyprfluidglass-protocol-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-protocol-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-protocol COMMAND hyprfluidglass-protocol-tests)

    add_executable(hyprfluidglass-runtime-tests tests/runtime_tests.cpp)
    target_include_directories(hyprfluidglass-runtime-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-runtime-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-runtime-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-runtime COMMAND hyprfluidglass-runtime-tests)
endif()
