#include "TestHarness.hpp"

#include "v2/compat/LegacyCompatibility.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

LegacyElement region(std::string id = "bar") {
  return {
      .sourceId = std::move(id),
      .attachment = LegacyAttachmentKind::Region,
      .output = "DP-1",
      .selector = "",
      .geometry = {10.0, 20.0, 400.0, 40.0},
      .material = {},
      .shape = RoundedRectShape{20.0},
      .transition = std::nullopt,
      .enabled = true,
      .under = false,
      .automaticWindowGeometry = false,
  };
}

WindowSnapshot window(std::string address = "0x1234") {
  return {
      .address = std::move(address),
      .objectToken = 7,
      .pid = 42,
      .initialClass = "org.example.Files",
      .currentClass = "org.example.Files",
      .initialTitle = "Files",
      .currentTitle = "Files",
      .globalGeometry = {100.0, 80.0, 800.0, 600.0},
      .rounding = 14.0,
      .roundingPower = 2.0,
      .opacity = 1.0,
      .mapped = true,
      .fadingOut = false,
      .readyToDelete = false,
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"region translates into the reserved v2 owner",
           [] {
             LegacyCompatibilityAdapter adapter;
             const std::array elements{region()};
             require(adapter.replace(true, elements, 3, 100).hasValue(),
                     "region replacement failed");
             const auto translated = adapter.snapshot({});
             require(translated.hasValue(), "region snapshot failed");
             const auto &session = translated.value().session;
             require(session.owner == LEGACY_COMPATIBILITY_OWNER &&
                         session.generation == 3 &&
                         session.targets.size() == 1 &&
                         session.materials.size() == 1,
                     "reserved compatibility state changed");
             const auto &target = session.targets.front();
             require(target.kind == TargetKind::Region &&
                         target.stage == RenderStage::PostWindows &&
                         std::get<RegionSelector>(target.selector).output ==
                             "DP-1",
                     "region target changed");
           }},
      Case{"identical legacy materials share one v2 material",
           [] {
             LegacyCompatibilityAdapter adapter;
             const std::array elements{region("one"), region("two")};
             require(adapter.replace(true, elements, 1, 10).hasValue(),
                     "deduplicated replacement failed");
             const auto translated = adapter.snapshot({});
             require(translated.hasValue() &&
                         translated.value().session.materials.size() == 1 &&
                         translated.value().session.targets.size() == 2,
                     "identical materials were not shared");
           }},
      Case{"layer selectors remain exact and derive their stage",
           [] {
             auto element = region();
             element.attachment = LegacyAttachmentKind::Layer;
             element.selector = "example-shell:bar:DP-1";
             LegacyCompatibilityAdapter adapter;
             require(
                 adapter.replace(true, std::array{element}, 1, 10).hasValue(),
                 "layer replacement failed");
             const auto translated = adapter.snapshot({});
             const auto &target = translated.value().session.targets.front();
             require(
                 target.kind == TargetKind::Layer && !target.stage &&
                     std::get<LayerSelector>(target.selector).namespaceName ==
                         element.selector,
                 "layer target changed");
           }},
      Case{"exact window selector gains identity guards and rounding",
           [] {
             auto element = region("files");
             element.attachment = LegacyAttachmentKind::Window;
             element.selector = "address:0x1234";
             element.geometry = {};
             element.under = true;
             element.automaticWindowGeometry = true;
             LegacyCompatibilityAdapter adapter;
             require(
                 adapter.replace(true, std::array{element}, 1, 10).hasValue(),
                 "window replacement failed");
             const auto translated = adapter.snapshot(std::array{window()});
             require(translated.hasValue(), "window snapshot failed");
             const auto &target = translated.value().session.targets.front();
             const auto &selector = std::get<WindowSelector>(target.selector);
             require(selector.address == "0x1234" && selector.pid == 42 &&
                         selector.initialClass == "org.example.Files" &&
                         std::get<RoundedRectShape>(target.shape).radius ==
                             14.0,
                     "window identity or automatic rounding changed");
           }},
      Case{"ambiguous window expressions fail closed",
           [] {
             auto element = region("files");
             element.attachment = LegacyAttachmentKind::Window;
             element.selector = "class:org\\.example\\..*";
             element.geometry = {};
             element.under = true;
             element.automaticWindowGeometry = true;
             LegacyCompatibilityAdapter adapter;
             require(
                 adapter.replace(true, std::array{element}, 1, 10).hasValue(),
                 "window expression replacement failed");
             auto second = window("0x5678");
             const auto translated =
                 adapter.snapshot(std::array{window(), second});
             require(!translated &&
                         translated.error().code == ErrorCode::UnresolvedTarget,
                     "ambiguous window selector chose an arbitrary target");
           }},
      Case{"window overlays and subregions remain unsupported",
           [] {
             auto overlay = region("overlay");
             overlay.attachment = LegacyAttachmentKind::Window;
             overlay.selector = "address:1234";
             overlay.geometry = {};
             LegacyCompatibilityAdapter adapter;
             require(!adapter.replace(true, std::array{overlay}, 1, 10),
                     "over-window legacy target reached v2");

             auto subregion = overlay;
             subregion.sourceId = "subregion";
             subregion.under = true;
             subregion.geometry = {0.0, 0.0, 400.0, 300.0};
             require(
                 adapter.replace(true, std::array{subregion}, 1, 10).hasValue(),
                 "subregion could not be staged for resolution");
             const auto translated = adapter.snapshot(std::array{window()});
             require(!translated && translated.error().code ==
                                        ErrorCode::UnsupportedTarget,
                     "window subregion silently expanded to the full window");
           }},
      Case{"failed replacement preserves the prior snapshot",
           [] {
             LegacyCompatibilityAdapter adapter;
             const std::array valid{region("stable")};
             require(adapter.replace(true, valid, 1, 10).hasValue(),
                     "initial replacement failed");
             auto invalid = region("");
             require(!adapter.replace(true, std::array{invalid}, 2, 20),
                     "invalid replacement succeeded");
             const auto translated = adapter.snapshot({});
             require(translated.hasValue() &&
                         translated.value().session.generation == 1 &&
                         translated.value().identities.front().sourceId ==
                             "stable",
                     "failed replacement changed compatibility state");
           }},
      Case{"disabled compatibility state publishes no authority",
           [] {
             LegacyCompatibilityAdapter adapter;
             require(
                 adapter.replace(false, std::array{region()}, 4, 50).hasValue(),
                 "disabled replacement failed");
             const auto translated = adapter.snapshot({});
             require(translated.hasValue() &&
                         translated.value().session.targets.empty() &&
                         translated.value().session.materials.empty(),
                     "disabled compatibility state retained live targets");
           }},
      Case{"clear releases compatibility authority without allocation",
           [] {
             LegacyCompatibilityAdapter adapter;
             require(
                 adapter.replace(true, std::array{region()}, 4, 50).hasValue(),
                 "initial replacement failed");
             adapter.clear();
             const auto translated = adapter.snapshot({});
             require(translated.hasValue() &&
                         translated.value().session.generation == 0 &&
                         translated.value().session.targets.empty() &&
                         translated.value().session.materials.empty(),
                     "clear retained compatibility authority");
           }},
  });
}
