#include "TestHarness.hpp"

#include "v2/targets/WindowBlurSuppressionPlan.hpp"

#include <array>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

WindowAttachmentState state(
    std::string owner,
    std::string targetId,
    std::uint64_t objectToken) {
    return {
        .identity = {
            .owner = std::move(owner),
            .targetId = std::move(targetId),
        },
        .objectToken = objectToken,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{
            "a newly attached window is claimed",
            [] {
                const std::array attached{state("hgs", "a", 7)};
                const auto plan = planWindowBlurSuppression({}, attached);
                require(plan.claim == std::vector<std::uint64_t>{7},
                        "the window should be claimed");
                require(plan.release.empty(), "nothing to release");
            },
        },
        Case{
            "a window that is still attached is not re-claimed",
            [] {
                const std::array attached{state("hgs", "a", 7)};
                const std::array held{std::uint64_t{7}};
                const auto plan = planWindowBlurSuppression(held, attached);
                require(plan.claim.empty(), "already claimed");
                require(plan.release.empty(), "still attached");
            },
        },
        Case{
            "a detached window is released",
            [] {
                const std::array held{std::uint64_t{7}};
                const auto plan = planWindowBlurSuppression(held, {});
                require(plan.claim.empty(), "nothing to claim");
                require(plan.release == std::vector<std::uint64_t>{7},
                        "the window should be released");
            },
        },
        Case{
            "a window stays claimed while any target still draws under it",
            [] {
                // Two targets, one window. Removing one must not release the
                // claim, or the surviving glass gets re-blurred into mush.
                const std::array attached{
                    state("hgs", "a", 7),
                    state("hgs", "b", 7),
                };
                const std::array held{std::uint64_t{7}};
                const auto both = planWindowBlurSuppression(held, attached);
                require(both.release.empty(), "two targets, still claimed");

                const std::array remaining{state("hgs", "b", 7)};
                const auto one = planWindowBlurSuppression(held, remaining);
                require(one.release.empty(), "one target left, still claimed");

                const auto none = planWindowBlurSuppression(held, {});
                require(none.release == std::vector<std::uint64_t>{7},
                        "last target gone, now released");
            },
        },
        Case{
            "claims and releases are computed together",
            [] {
                const std::array attached{state("hgs", "a", 9)};
                const std::array held{std::uint64_t{7}};
                const auto plan = planWindowBlurSuppression(held, attached);
                require(plan.claim == std::vector<std::uint64_t>{9},
                        "the new window is claimed");
                require(plan.release == std::vector<std::uint64_t>{7},
                        "the old window is released");
            },
        },
        Case{
            "a zero object token never claims a window",
            [] {
                // Zero names no window; claiming on it would suppress blur for
                // whatever token later occupies the slot.
                const std::array attached{state("hgs", "a", 0)};
                const auto plan = planWindowBlurSuppression({}, attached);
                require(plan.claim.empty(), "zero is not a window");
            },
        },
        Case{
            "output is sorted and free of duplicates",
            [] {
                const std::array attached{
                    state("hgs", "a", 9),
                    state("hgs", "b", 3),
                    state("hgs", "c", 9),
                };
                const auto plan = planWindowBlurSuppression({}, attached);
                require(plan.claim == std::vector<std::uint64_t>{3, 9},
                        "sorted, deduplicated");
            },
        },
        Case{
            "a repeated held token is released once",
            [] {
                const std::array held{
                    std::uint64_t{7},
                    std::uint64_t{7},
                };
                const auto plan = planWindowBlurSuppression(held, {});
                require(plan.release == std::vector<std::uint64_t>{7},
                        "released once");
            },
        },
    });
}
