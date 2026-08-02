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
        // Layer geometry must be sampled from the LIVE animated value.
        // CLayerSurface::m_geometry is the destination the layout produced;
        // Hyprland's own renderer reads position/size(GEOMETRIC_CURRENT), and
        // so must the catalog. Reading the destination would snap glass to the
        // final position on the first frame of an open or move animation and
        // leave it there while the surface slides — the single mistake this
        // design is most likely to make.
        Case{"the layer catalog samples animated geometry, not the destination", [] {
            const auto catalog = source(
                "src/v2/targets/HyprlandLayerCatalog.cpp");
            const auto body = functionBody(
                catalog,
                "HyprlandLayerCatalog::allSnapshots",
                "void HyprlandLayerCatalog::clear");

            require(
                body.find("GEOMETRIC_CURRENT") != std::string_view::npos,
                "layer geometry is no longer sampled from the animated value");
            require(
                body.find("GEOMETRIC_GOAL") == std::string_view::npos,
                "layer geometry must not be sampled from the destination");
            require(
                body.find("m_geometry") == std::string_view::npos,
                "m_geometry is the layout destination, not the live box");
        }},
        Case{"the layer catalog derives its content rect from the input region", [] {
            // effectiveInputRegion() already returns the whole surface when the
            // client left the region infinite, and clips it otherwise. Reading
            // m_current.input directly would mishandle the default.
            const auto catalog = source(
                "src/v2/targets/HyprlandLayerCatalog.cpp");
            const auto body = functionBody(
                catalog,
                "std::optional<Rect> contentGeometryFor",
                "Result<std::vector<LayerSurfaceSnapshot>> unavailable");

            require(
                body.find("effectiveInputRegion") != std::string_view::npos,
                "the content rect no longer comes from the input region");
            require(
                body.find("m_current.input") == std::string_view::npos,
                "the raw input region mishandles the infinite default");
        }},
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
            // The reporting logic lives in the extracted, unit-tested
            // reconcilePresentationReadiness; the controller must still route
            // every refresh through it.
            const auto controller = source(
                "src/v2/runtime/HyprlandGlassSceneController.cpp");
            const auto reconcile = functionBody(
                controller,
                "HyprlandGlassSceneController::reconcileReadiness",
                "HyprlandGlassSceneController::recordFailure");
            require(
                reconcile.find("reconcilePresentationReadiness") !=
                    std::string_view::npos,
                "controller does not route readiness through the reconciler");

            const auto scene = source("src/v2/render/PresentationScene.cpp");
            require(
                scene.find("scene.inactive") != std::string_view::npos,
                "inactive targets are never reported to readiness");
            require(
                scene.find("scene.suppressed") != std::string_view::npos,
                "suppressed targets are never reported to readiness");
            require(
                scene.find("ReadinessState::Inactive") !=
                    std::string_view::npos,
                "inactive targets are not given a distinct readiness state");
        }},
    });
}
