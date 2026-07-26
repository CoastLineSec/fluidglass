#include "TestHarness.hpp"

#include "v2/render/DirectScanoutLeasePlan.hpp"

#include <array>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

DirectScanoutLease lease(std::string output = "DP-1",
                         std::uint64_t objectToken = 7) {
  return {
      .output = std::move(output),
      .objectToken = objectToken,
  };
}

} // namespace

int main() {
  return hfg::test::run({
      Case{"unchanged output lease is retained",
           [] {
             const std::array current{lease()};
             const std::array desired{lease()};
             const auto plan = planDirectScanoutLeases(current, desired);
             require(plan.hasValue() &&
                         plan.value().retain == std::vector{lease()} &&
                         plan.value().acquire.empty() &&
                         plan.value().release.empty(),
                     "stable lease was not retained");
           }},
      Case{"output generation replacement acquires before release",
           [] {
             const std::array current{lease("DP-1", 7)};
             const std::array desired{lease("DP-1", 9)};
             const auto plan = planDirectScanoutLeases(current, desired);
             require(plan.hasValue() && plan.value().retain.empty() &&
                         plan.value().acquire ==
                             std::vector{lease("DP-1", 9)} &&
                         plan.value().release == std::vector{lease("DP-1", 7)},
                     "changed output generation was not replaced");
           }},
      Case{"independent output changes remain scoped",
           [] {
             const std::array current{lease("DP-1", 7), lease("HDMI-A-1", 8)};
             const std::array desired{lease("DP-1", 7), lease("eDP-1", 10)};
             const auto plan = planDirectScanoutLeases(current, desired);
             require(
                 plan.hasValue() &&
                     plan.value().retain == std::vector{lease("DP-1", 7)} &&
                     plan.value().acquire == std::vector{lease("eDP-1", 10)} &&
                     plan.value().release == std::vector{lease("HDMI-A-1", 8)},
                 "output-scoped lease changes were conflated");
           }},
      Case{"duplicate output identity is rejected",
           [] {
             const std::array desired{lease("DP-1", 7), lease("DP-1", 8)};
             const auto plan = planDirectScanoutLeases(
                 std::span<const DirectScanoutLease>{}, desired);
             require(!plan && plan.error().path == "desired[1].output",
                     "duplicate output lease was accepted");
           }},
      Case{"duplicate compositor object identity is rejected",
           [] {
             const std::array desired{lease("DP-1", 7), lease("HDMI-A-1", 7)};
             const auto plan = planDirectScanoutLeases(
                 std::span<const DirectScanoutLease>{}, desired);
             require(!plan && plan.error().path == "desired[1].object_token",
                     "duplicate output object token was accepted");
           }},
      Case{"malformed leases fail before reconciliation",
           [] {
             const std::array emptyName{lease("", 7)};
             const std::array emptyToken{lease("DP-1", 0)};
             require(!planDirectScanoutLeases(
                         std::span<const DirectScanoutLease>{}, emptyName) &&
                         !planDirectScanoutLeases(
                             std::span<const DirectScanoutLease>{}, emptyToken),
                     "malformed lease reached reconciliation");
           }},
  });
}
