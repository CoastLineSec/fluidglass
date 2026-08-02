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

add_library(hyprfluidglass SHARED
    src/main.cpp
    src/v2/render/HyprlandCaptureExecutor.cpp
    src/v2/render/HyprlandCaptureEnvironment.cpp
    src/v2/render/HyprlandCaptureFormat.cpp
    src/v2/render/HyprlandCaptureResource.cpp
    src/v2/render/HyprlandCaptureResourceManager.cpp
    src/v2/render/HyprlandDirectScanoutInhibitor.cpp
    src/v2/render/HyprlandGlassBlur.cpp
    src/v2/render/HyprlandGlassDrawExecutor.cpp
    src/v2/render/HyprlandGlassPassCoordinator.cpp
    src/v2/render/HyprlandGlassShader.cpp
    src/v2/render/HyprlandOutputCatalog.cpp
    src/v2/render/HyprlandRenderStageAdapter.cpp
    src/v2/render/HyprlandStateGuard.cpp
    src/v2/runtime/HyprlandGlassSceneController.cpp
    src/v2/targets/HyprlandLayerCatalog.cpp
    src/v2/targets/HyprlandWindowAttachmentManager.cpp
    src/v2/targets/HyprlandWindowDecoration.cpp
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
)
set_target_properties(hyprfluidglass PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

install(TARGETS hyprfluidglass LIBRARY DESTINATION lib/hyprland)

add_library(hyprfluidglass-v2-core STATIC
    src/v2/compat/LegacyCompatibility.cpp
    src/v2/core/OpaqueId.cpp
    src/v2/ipc/Protocol.cpp
    src/v2/model/Config.cpp
    src/v2/model/Material.cpp
    src/v2/model/Readiness.cpp
    src/v2/model/Session.cpp
    src/v2/model/Target.cpp
    src/v2/render/CaptureBlit.cpp
    src/v2/render/CaptureCache.cpp
    src/v2/render/CaptureEnvironment.cpp
    src/v2/render/CaptureExecutionTracker.cpp
    src/v2/render/CapturePlan.cpp
    src/v2/render/CaptureResourcePlan.cpp
    src/v2/render/CaptureScene.cpp
    src/v2/render/DirectScanoutLeasePlan.cpp
    src/v2/render/Geometry.cpp
    src/v2/render/GlassDrawPlan.cpp
    src/v2/render/GlassFramePlan.cpp
    src/v2/render/GlassRenderScene.cpp
    src/v2/render/GlassShaderSource.cpp
    src/v2/render/GlassUniformPayload.cpp
    src/v2/render/MaterialSampling.cpp
    src/v2/render/OutputGeneration.cpp
    src/v2/render/PresentationScene.cpp
    src/v2/render/RenderStageScheduler.cpp
    src/v2/render/ShapeMotion.cpp
    src/v2/render/TransitionMotion.cpp
    src/v2/runtime/Runtime.cpp
    src/v2/runtime/LiveScenePlan.cpp
    src/v2/targets/Attachment.cpp
    src/v2/targets/LayerAdapter.cpp
    src/v2/targets/LayerRuleResolver.cpp
    src/v2/targets/MaterialResolver.cpp
    src/v2/targets/RegionAdapter.cpp
    src/v2/targets/TargetResolver.cpp
    src/v2/targets/TargetMotion.cpp
    src/v2/targets/TargetPrecedence.cpp
    src/v2/targets/TargetScene.cpp
    src/v2/targets/WindowAttachmentPlan.cpp
    src/v2/targets/WindowBlurSuppressionPlan.cpp
    src/v2/targets/WindowAdapter.cpp
    src/v2/targets/WindowRuleResolver.cpp
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
    # One declaration per test binary. SOURCES are the test translation
    # units; LINK names the libraries (omit for header-only tests);
    # SOURCE_DIR bakes the repository path in for source-contract tests;
    # INCLUDE_SRC exposes src/ headers directly.
    function(hfg_test name)
        cmake_parse_arguments(HFG "SOURCE_DIR;INCLUDE_SRC" "" "SOURCES;LINK;DEFINE" ${ARGN})
        set(target hyprfluidglass-${name}-tests)
        add_executable(${target} ${HFG_SOURCES})
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        if(HFG_INCLUDE_SRC)
            target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        endif()
        if(HFG_LINK)
            target_link_libraries(${target} PRIVATE ${HFG_LINK})
        endif()
        if(HFG_SOURCE_DIR)
            target_compile_definitions(${target} PRIVATE
                HYPRFLUIDGLASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
        if(HFG_DEFINE)
            target_compile_definitions(${target} PRIVATE ${HFG_DEFINE})
        endif()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME hyprfluidglass-${name} COMMAND ${target})
    endfunction()

    hfg_test(core SOURCES tests/core_tests.cpp INCLUDE_SRC)
    hfg_test(legacy-compatibility SOURCES tests/legacy_compatibility_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(direct-scanout-lease-plan SOURCES tests/direct_scanout_lease_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(hyprland-render-lifecycle-contract SOURCES tests/hyprland_render_lifecycle_contract_tests.cpp SOURCE_DIR)
    hfg_test(opaque-id SOURCES tests/opaque_id_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(lua-config SOURCES tests/lua_config_tests.cpp LINK hyprfluidglass-v2-lua)
    hfg_test(material SOURCES tests/material_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(material-sampling SOURCES tests/material_sampling_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(presentation-scene SOURCES tests/presentation_scene_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(target SOURCES tests/target_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(session SOURCES tests/session_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(config SOURCES tests/config_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(readiness SOURCES tests/readiness_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(output-generation SOURCES tests/output_generation_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(geometry SOURCES tests/geometry_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(glass-draw-plan SOURCES tests/glass_draw_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(glass-frame-plan SOURCES tests/glass_frame_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(glass-render-scene SOURCES tests/glass_render_scene_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(presentation-resource-cache SOURCES tests/presentation_resource_cache_tests.cpp INCLUDE_SRC)
    hfg_test(glass-shader-source SOURCES tests/glass_shader_source_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(glass-uniform-payload SOURCES tests/glass_uniform_payload_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-plan SOURCES tests/capture_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-scene SOURCES tests/capture_scene_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-cache SOURCES tests/capture_cache_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-resource-plan SOURCES tests/capture_resource_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-environment SOURCES tests/capture_environment_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(capture-execution-tracker SOURCES tests/capture_execution_tracker_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(render-stage-scheduler SOURCES tests/render_stage_scheduler_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(transition-motion SOURCES tests/transition_motion_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(shape-motion SOURCES tests/shape_motion_tests.cpp LINK hyprfluidglass-v2-core)
    # Compiles HyprlandCaptureFormat.cpp directly: the shipped .so is not a
    # linkable library, and this is the one Hyprland-coupled unit small
    # enough to test against the real headers.
    hfg_test(hyprland-capture-format SOURCES tests/hyprland_capture_format_tests.cpp src/v2/render/HyprlandCaptureFormat.cpp LINK hyprfluidglass-v2-core PkgConfig::HYPRLAND INCLUDE_SRC)
    hfg_test(capture-blit SOURCES tests/capture_blit_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(attachment SOURCES tests/attachment_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(region-adapter SOURCES tests/region_adapter_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(layer-adapter SOURCES tests/layer_adapter_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(layer-rule-resolver SOURCES tests/layer_rule_resolver_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(material-resolver SOURCES tests/material_resolver_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(window-adapter SOURCES tests/window_adapter_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(window-attachment-plan SOURCES tests/window_attachment_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(window-blur-suppression-plan SOURCES tests/window_blur_suppression_plan_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(window-rule-resolver SOURCES tests/window_rule_resolver_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(target-resolver SOURCES tests/target_resolver_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(target-motion SOURCES tests/target_motion_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(target-precedence SOURCES tests/target_precedence_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(target-scene SOURCES tests/target_scene_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(protocol SOURCES tests/protocol_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(runtime SOURCES tests/runtime_tests.cpp LINK hyprfluidglass-v2-core)
    hfg_test(live-scene-plan SOURCES tests/live_scene_plan_tests.cpp LINK hyprfluidglass-v2-core)
endif()
