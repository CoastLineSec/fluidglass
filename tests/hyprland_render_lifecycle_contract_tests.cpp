#include "TestHarness.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

using hfg::test::Case;
using hfg::test::require;

namespace {

std::string source(std::string_view relativePath) {
    const auto path =
        std::filesystem::path(HYPRFLUIDGLASS_SOURCE_DIR) / relativePath;
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("failed to read " + path.string());
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>(),
    };
}

std::string_view functionBody(
    const std::string& text,
    std::string_view start,
    std::string_view next) {
    const auto begin = text.find(start);
    require(begin != std::string::npos, "function start was not found");
    const auto end = text.find(next, begin + start.size());
    require(end != std::string::npos, "function end was not found");
    return std::string_view(text).substr(begin, end - begin);
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"scene reconciliation stays outside an active render frame", [] {
            const auto controller = source(
                "src/v2/runtime/HyprlandGlassSceneController.cpp");
            const auto prechecks = functionBody(
                controller,
                "HyprlandGlassSceneController::onPreChecks",
                "HyprlandGlassSceneController::onRenderStage");
            const auto renderStage = functionBody(
                controller,
                "HyprlandGlassSceneController::onRenderStage",
                "HyprlandGlassSceneController::refreshResolvedScene");

            require(
                prechecks.find("return refresh(nowMs)") !=
                    std::string_view::npos,
                "pre-checks no longer perform authoritative scene refresh");
            require(
                renderStage.find("refreshResolvedScene") ==
                    std::string_view::npos,
                "render-stage callback can reconcile compositor resources");
            require(
                renderStage.find("prepareRenderScene") !=
                    std::string_view::npos,
                "render begin no longer prepares the current scene");
        }},
        Case{"direct-scanout mutation rejects an active render frame", [] {
            const auto inhibitor = source(
                "src/v2/render/HyprlandDirectScanoutInhibitor.cpp");
            const auto reconcile = functionBody(
                inhibitor,
                "HyprlandDirectScanoutInhibitor::reconcile",
                "HyprlandDirectScanoutInhibitor::leases");

            require(
                reconcile.find("m_renderData.pMonitor") !=
                    std::string_view::npos,
                "direct-scanout reconciliation lacks an active-frame guard");
            require(
                reconcile.find("direct-scanout.render-frame") !=
                    std::string_view::npos,
                "active-frame rejection lacks a stable diagnostic");
            require(
                reconcile.find("m_renderData.pMonitor") <
                    reconcile.find("Pointer::mgr"),
                "cursor backend access occurs before active-frame rejection");
        }},
        Case{"readiness reconciliation reports the drawable-nothing partitions", [] {
            const auto controller = source(
                "src/v2/runtime/HyprlandGlassSceneController.cpp");
            const auto reconcile = functionBody(
                controller,
                "HyprlandGlassSceneController::reconcileReadiness",
                "HyprlandGlassSceneController::recordFailure");

            require(
                reconcile.find("m_presentations.inactive") !=
                    std::string_view::npos,
                "inactive targets are never reported to readiness");
            require(
                reconcile.find("m_presentations.suppressed") !=
                    std::string_view::npos,
                "suppressed targets are never reported to readiness");
            require(
                reconcile.find("ReadinessState::Inactive") !=
                    std::string_view::npos,
                "inactive targets are not given a distinct readiness state");
        }},
    });
}
