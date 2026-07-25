#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace hfg::v2 {

struct WindowAttachmentState {
    TargetIdentity identity;
    std::uint64_t  objectToken = 0;

    friend bool operator==(const WindowAttachmentState&, const WindowAttachmentState&) = default;
    friend auto operator<=>(const WindowAttachmentState&, const WindowAttachmentState&) = default;
};

struct WindowAttachmentPlan {
    std::vector<WindowAttachmentState> retain;
    std::vector<WindowAttachmentState> add;
    std::vector<WindowAttachmentState> remove;

    friend bool operator==(const WindowAttachmentPlan&, const WindowAttachmentPlan&) = default;
};

[[nodiscard]] Result<WindowAttachmentPlan>
planWindowAttachments(
    std::span<const WindowAttachmentState> current,
    std::span<const WindowAttachmentState> desired);

} // namespace hfg::v2
