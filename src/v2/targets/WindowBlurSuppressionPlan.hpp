#pragma once

#include "v2/targets/WindowAttachmentPlan.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

/**
 * Which windows must have Hyprland's own blur suppressed.
 *
 * Hyprland auto-blurs every translucent window whenever `decoration:blur:enabled`
 * is on — no rule required — and that kawase pass runs AFTER the glass pass.
 * A window drawn over finished glass therefore has the material re-processed
 * when floating, and replaced outright by the precomputed background-blur
 * framebuffer when tiled. Layer surfaces are not affected: they blur only when
 * a layer rule says so.
 *
 * The engine consequently claims every window it draws under and releases the
 * claim when the last glass element leaves. The claim is reversible and set at
 * the highest priority so a user rule cannot silently defeat it.
 *
 * Suppression is a projection of the current attachments rather than a ledger
 * kept alongside them, so the two cannot drift: a window is claimed exactly
 * while at least one decoration is attached to it.
 */
struct WindowBlurSuppressionPlan {
    /** Object tokens whose windows should have blur suppressed now. */
    std::vector<std::uint64_t> claim;
    /** Object tokens whose windows should have their suppression released. */
    std::vector<std::uint64_t> release;

    friend bool operator==(const WindowBlurSuppressionPlan&,
                           const WindowBlurSuppressionPlan&) = default;
};

/**
 * Diffs the windows currently suppressed against the windows still attached.
 *
 * `attached` may name the same window more than once — several targets can draw
 * under one window — and a window stays claimed while any of them remain.
 * Both output vectors are sorted and free of duplicates.
 */
[[nodiscard]] WindowBlurSuppressionPlan planWindowBlurSuppression(
    std::span<const std::uint64_t> suppressed,
    std::span<const WindowAttachmentState> attached);

} // namespace hfg::v2
