#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/model/Target.hpp"

#include <limits>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

TargetInput regionTarget() {
    return {
        .id = "preview",
        .kind = TargetKind::Region,
        .material = {.source = MaterialSource::Session, .name = "fluid"},
        .shape = RoundedRectShape{.radius = 20.0},
        .selector = RegionSelector{.output = "DP-1"},
        .geometry = Rect{.x = 10.0, .y = 20.0, .width = 400.0, .height = 300.0},
        .stage = RenderStage::PostWindows,
        .enabled = true,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"region target", [] {
            const auto result = validateTarget(regionTarget());
            require(result.hasValue(), "valid region target was rejected");
            require(result.value().kind == TargetKind::Region, "target kind changed");
        }},
        Case{"window identity", [] {
            TargetInput input{
                .id = "files",
                .kind = TargetKind::Window,
                .material = {.source = MaterialSource::Config, .name = "fluid"},
                .shape = RoundedRectShape{.radius = 12.0},
                .selector = WindowSelector{
                    .address = "ABCDEF",
                    .pid = 42,
                    .initialClass = std::nullopt,
                },
                .geometry = std::nullopt,
                .stage = std::nullopt,
            };
            const auto result = validateTarget(std::move(input));
            require(result.hasValue(), "valid window target was rejected");
            const auto& selector = std::get<WindowSelector>(result.value().selector);
            require(selector.address == "0xabcdef", "window address was not normalized");
        }},
        Case{"window guard required", [] {
            TargetInput input{
                .id = "files",
                .kind = TargetKind::Window,
                .material = {.source = MaterialSource::Config, .name = "fluid"},
                .shape = RoundedRectShape{},
                .selector = WindowSelector{
                    .address = "0x1234",
                    .pid = std::nullopt,
                    .initialClass = std::nullopt,
                },
                .geometry = std::nullopt,
                .stage = std::nullopt,
            };
            require(!validateTarget(std::move(input)), "unguarded window address must fail");
        }},
        Case{"selector kind must match", [] {
            auto input = regionTarget();
            input.kind = TargetKind::Layer;
            require(!validateTarget(std::move(input)), "mismatched selector kind must fail");
        }},
        Case{"layer geometry optional", [] {
            TargetInput input{
                .id = "bar",
                .kind = TargetKind::Layer,
                .material = {.source = MaterialSource::Session, .name = "bar"},
                .shape = RoundedRectShape{.radius = 22.0},
                .selector = LayerSelector{.namespaceName = "example-shell:bar:primary"},
                .geometry = std::nullopt,
                .stage = std::nullopt,
            };
            require(validateTarget(input).hasValue(), "whole-surface layer target must be valid");
            input.geometry = Rect{.x = 0.0, .y = 0.0, .width = 1000.0, .height = 44.0};
            require(validateTarget(input).hasValue(), "surface-local layer geometry must be valid");
        }},
        Case{"geometry must be finite and positive", [] {
            auto input = regionTarget();
            input.geometry->width = 0.0;
            require(!validateTarget(input), "zero width must fail");
            input.geometry->width = 100.0;
            input.geometry->x = std::numeric_limits<double>::quiet_NaN();
            require(!validateTarget(input), "NaN coordinate must fail");
            input.geometry->x = 0.0;
            input.geometry->height = std::numeric_limits<double>::infinity();
            require(!validateTarget(input), "infinite height must fail");
        }},
        Case{"shape validation", [] {
            auto input = regionTarget();
            input.shape = RingShape{.outerRadius = 20.0, .thickness = 0.0};
            require(!validateTarget(input), "zero ring thickness must fail");
            input.shape = CompoundShape{};
            require(!validateTarget(input), "empty compound must fail");
            CompoundShape compound;
            compound.parts.resize(Limits::MAX_COMPOUND_PARTS + 1U);
            input.shape = std::move(compound);
            const auto result = validateTarget(input);
            require(!result, "over-limit compound must fail");
            require(result.error().code == ErrorCode::ResourceLimited, "part limit must report resource-limited");
        }},
        Case{"region requires stage and geometry", [] {
            auto input = regionTarget();
            input.stage = std::nullopt;
            require(!validateTarget(input), "region without stage must fail");
            input = regionTarget();
            input.geometry = std::nullopt;
            require(!validateTarget(input), "region without geometry must fail");
        }},
        Case{"reserved identifiers rejected", [] {
            auto input = regionTarget();
            input.id = "_hfg_preview";
            require(!validateTarget(input), "reserved target id must fail");
            input = regionTarget();
            input.material.name = "bad/name";
            require(!validateTarget(input), "invalid material reference must fail");
        }},
    });
}
