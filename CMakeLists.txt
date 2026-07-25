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
    src/v2/render/HyprlandCaptureExecutor.cpp
    src/v2/render/HyprlandCaptureResource.cpp
    src/v2/render/HyprlandStateGuard.cpp
    src/v2/targets/HyprlandLayerCatalog.cpp
    src/v2/targets/HyprlandWindowCatalog.cpp
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
    src/v2/render/CaptureBlit.cpp
    src/v2/render/CaptureCache.cpp
    src/v2/render/CapturePlan.cpp
    src/v2/render/Geometry.cpp
    src/v2/render/OutputGeneration.cpp
    src/v2/runtime/Runtime.cpp
    src/v2/targets/Attachment.cpp
    src/v2/targets/LayerAdapter.cpp
    src/v2/targets/RegionAdapter.cpp
    src/v2/targets/WindowAdapter.cpp
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
target_link_libraries(hyprfluidglass PRIVATE hyprfluidglass-v2-lua)

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

    add_executable(hyprfluidglass-output-generation-tests tests/output_generation_tests.cpp)
    target_include_directories(hyprfluidglass-output-generation-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-output-generation-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-output-generation-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-output-generation COMMAND hyprfluidglass-output-generation-tests)

    add_executable(hyprfluidglass-geometry-tests tests/geometry_tests.cpp)
    target_include_directories(hyprfluidglass-geometry-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-geometry-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-geometry-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-geometry COMMAND hyprfluidglass-geometry-tests)

    add_executable(hyprfluidglass-capture-plan-tests tests/capture_plan_tests.cpp)
    target_include_directories(hyprfluidglass-capture-plan-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-capture-plan-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-capture-plan-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-capture-plan COMMAND hyprfluidglass-capture-plan-tests)

    add_executable(hyprfluidglass-capture-cache-tests tests/capture_cache_tests.cpp)
    target_include_directories(hyprfluidglass-capture-cache-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-capture-cache-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-capture-cache-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-capture-cache COMMAND hyprfluidglass-capture-cache-tests)

    add_executable(hyprfluidglass-capture-blit-tests tests/capture_blit_tests.cpp)
    target_include_directories(hyprfluidglass-capture-blit-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-capture-blit-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-capture-blit-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-capture-blit COMMAND hyprfluidglass-capture-blit-tests)

    add_executable(hyprfluidglass-attachment-tests tests/attachment_tests.cpp)
    target_include_directories(hyprfluidglass-attachment-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-attachment-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-attachment-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-attachment COMMAND hyprfluidglass-attachment-tests)

    add_executable(hyprfluidglass-region-adapter-tests tests/region_adapter_tests.cpp)
    target_include_directories(hyprfluidglass-region-adapter-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-region-adapter-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-region-adapter-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-region-adapter COMMAND hyprfluidglass-region-adapter-tests)

    add_executable(hyprfluidglass-layer-adapter-tests tests/layer_adapter_tests.cpp)
    target_include_directories(hyprfluidglass-layer-adapter-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-layer-adapter-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-layer-adapter-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-layer-adapter COMMAND hyprfluidglass-layer-adapter-tests)

    add_executable(hyprfluidglass-window-adapter-tests tests/window_adapter_tests.cpp)
    target_include_directories(hyprfluidglass-window-adapter-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hyprfluidglass-window-adapter-tests PRIVATE hyprfluidglass-v2-core)
    target_compile_options(hyprfluidglass-window-adapter-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME hyprfluidglass-window-adapter COMMAND hyprfluidglass-window-adapter-tests)

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
