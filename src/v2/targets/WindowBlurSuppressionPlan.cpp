#include "v2/targets/WindowBlurSuppressionPlan.hpp"

#include <algorithm>
#include <ranges>

namespace hfg::v2 {

namespace {

std::vector<std::uint64_t> sortedUnique(std::vector<std::uint64_t> values) {
    std::ranges::sort(values);
    const auto duplicates = std::ranges::unique(values);
    values.erase(duplicates.begin(), duplicates.end());
    return values;
}

} // namespace

WindowBlurSuppressionPlan planWindowBlurSuppression(
    std::span<const std::uint64_t> suppressed,
    std::span<const WindowAttachmentState> attached) {
    std::vector<std::uint64_t> wanted;
    wanted.reserve(attached.size());
    for (const auto& state : attached) {
        // A zero token never names a window; claiming on it would suppress blur
        // for whatever token later occupies the slot.
        if (state.objectToken != 0)
            wanted.push_back(state.objectToken);
    }
    wanted = sortedUnique(std::move(wanted));

    const auto held = sortedUnique(
        std::vector<std::uint64_t>(suppressed.begin(), suppressed.end()));

    WindowBlurSuppressionPlan plan;
    std::ranges::set_difference(wanted, held, std::back_inserter(plan.claim));
    std::ranges::set_difference(held, wanted, std::back_inserter(plan.release));
    return plan;
}

} // namespace hfg::v2
