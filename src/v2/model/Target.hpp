#pragma once

#include "v2/core/Result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hfg::v2 {

/**
 * Owner recorded on targets resolved from the Lua configuration. Config-rule
 * glass never passes through a session, so this owner marks readiness records
 * whose lifetime follows the resolved scene rather than a session generation.
 */
inline constexpr std::string_view CONFIG_TARGET_OWNER = "config";


enum class TargetKind {
    Window,
    Layer,
    Region,
};

enum class MaterialSource {
    Config,
    Session,
};

enum class RenderStage {
    PostWallpaper,
    PreWindow,
    PostWindows,
    PostLayer,
};

enum class TransitionPhase {
    Enter,
    Exit,
};

enum class TransitionEdge {
    Top,
    Bottom,
    Left,
    Right,
};

struct MaterialReference {
    MaterialSource source = MaterialSource::Session;
    std::string    name;

    friend bool operator==(const MaterialReference&, const MaterialReference&) = default;
};

struct Rect {
    double x      = 0.0;
    double y      = 0.0;
    double width  = 0.0;
    double height = 0.0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

struct RoundedRectShape {
    double radius = 0.0;

    friend bool operator==(const RoundedRectShape&, const RoundedRectShape&) = default;
};

struct RingShape {
    double outerRadius = 0.0;
    double thickness   = 0.0;

    friend bool operator==(const RingShape&, const RingShape&) = default;
};

struct CornerRadii {
    double topLeft     = 0.0;
    double topRight    = 0.0;
    double bottomRight = 0.0;
    double bottomLeft  = 0.0;

    friend bool operator==(const CornerRadii&, const CornerRadii&) = default;
};

struct CubicBezierSegment {
    double control1X = 0.0;
    double control1Y = 0.0;
    double control2X = 0.0;
    double control2Y = 0.0;
    double endX      = 1.0;
    double endY      = 1.0;

    friend bool operator==(const CubicBezierSegment&, const CubicBezierSegment&) = default;
};

struct Transition {
    std::string                     id;
    TransitionPhase                 phase = TransitionPhase::Enter;
    TransitionEdge                  edge = TransitionEdge::Top;
    std::uint64_t                   durationMs = 0;
    std::uint64_t                   elapsedMs = 0;
    double                          travel = 0.0;
    std::vector<CubicBezierSegment> easing;

    friend bool operator==(const Transition&, const Transition&) = default;
};

struct PartTransition {
    Transition motion;
    double     protrusion = 0.0;

    friend bool operator==(const PartTransition&, const PartTransition&) = default;
};

struct CompoundBase {
    CornerRadii corners;

    friend bool operator==(const CompoundBase&, const CompoundBase&) = default;
};

struct CompoundCutout {
    Rect        rect;
    CornerRadii corners;

    friend bool operator==(const CompoundCutout&, const CompoundCutout&) = default;
};

struct CompoundPart {
    Rect                rect;
    CornerRadii         corners;
    CornerRadii         junctions;
    std::optional<Rect> materialExtent;
    std::optional<PartTransition> transition;
    double              opacity = 1.0;

    friend bool operator==(const CompoundPart&, const CompoundPart&) = default;
};

struct CompoundShape {
    std::optional<CompoundBase>   base;
    std::optional<CompoundCutout> cutout;
    std::vector<CompoundPart>     parts;
    std::vector<Rect>             connectors;
    double                        connectorCurve = 0.0;

    friend bool operator==(const CompoundShape&, const CompoundShape&) = default;
};

using Shape = std::variant<RoundedRectShape, RingShape, CompoundShape>;

struct WindowSelector {
    std::string                address;
    std::optional<std::int64_t> pid;
    std::optional<std::string> initialClass;

    friend bool operator==(const WindowSelector&, const WindowSelector&) = default;
};

struct LayerSelector {
    std::string namespaceName;

    friend bool operator==(const LayerSelector&, const LayerSelector&) = default;
};

struct RegionSelector {
    std::string output;

    friend bool operator==(const RegionSelector&, const RegionSelector&) = default;
};

using TargetSelector = std::variant<WindowSelector, LayerSelector, RegionSelector>;

struct TargetInput {
    std::string                 id;
    TargetKind                  kind = TargetKind::Region;
    MaterialReference           material;
    Shape                       shape = RoundedRectShape{};
    TargetSelector              selector = RegionSelector{};
    std::optional<Rect>         geometry;
    std::optional<RenderStage>  stage;
    std::optional<Transition>   transition;
    bool                        enabled = true;
};

struct Target {
    std::string                id;
    TargetKind                 kind = TargetKind::Region;
    MaterialReference          material;
    Shape                      shape;
    TargetSelector             selector;
    std::optional<Rect>        geometry;
    std::optional<RenderStage> stage;
    std::optional<Transition>  transition;
    bool                       enabled = true;

    friend bool operator==(const Target&, const Target&) = default;
};

[[nodiscard]] Result<Target> validateTarget(TargetInput input);

} // namespace hfg::v2
