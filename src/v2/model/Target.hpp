#pragma once

#include "v2/core/Result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hfg::v2 {

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
    bool                       enabled = true;

    friend bool operator==(const Target&, const Target&) = default;
};

[[nodiscard]] Result<Target> validateTarget(TargetInput input);

} // namespace hfg::v2
