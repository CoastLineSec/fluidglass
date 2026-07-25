#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Readiness.hpp"
#include "v2/model/Target.hpp"
#include "v2/targets/Attachment.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace hfg::v2 {

struct WindowSnapshot {
    std::string   address;
    std::uint64_t objectToken = 0;
    std::int64_t  pid = 0;
    std::string   initialClass;
    std::string   currentClass;
    std::string   initialTitle;
    std::string   currentTitle;
    Rect          globalGeometry;
    double        rounding = 0.0;
    double        roundingPower = 2.0;
    double        opacity = 1.0;
    bool          mapped = false;
    bool          fadingOut = false;
    bool          readyToDelete = false;

    friend bool operator==(const WindowSnapshot&, const WindowSnapshot&) = default;
};

[[nodiscard]] Result<std::optional<ResolvedAttachment>>
resolveWindowAttachment(
    TargetIdentity identity,
    const Target& target,
    std::span<const WindowSnapshot> windows);

} // namespace hfg::v2
