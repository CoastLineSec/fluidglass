#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Material.hpp"
#include "v2/model/Session.hpp"
#include "v2/model/Target.hpp"
#include "v2/targets/WindowAdapter.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hfg::v2 {

enum class LegacyAttachmentKind {
  Region,
  Layer,
  Window,
};

struct LegacyElement {
  std::string sourceId;
  LegacyAttachmentKind attachment = LegacyAttachmentKind::Region;
  std::string output;
  std::string selector;
  Rect geometry;
  MaterialInput material;
  Shape shape = RoundedRectShape{};
  std::optional<Transition> transition;
  bool enabled = true;
  bool under = false;
  bool automaticWindowGeometry = false;
};

struct LegacyIdentityMapping {
  std::string sourceId;
  std::string targetId;

  friend bool operator==(const LegacyIdentityMapping &,
                         const LegacyIdentityMapping &) = default;
};

struct LegacyCompatibilitySnapshot {
  SessionSnapshot session;
  std::vector<LegacyIdentityMapping> identities;
};

class LegacyCompatibilityAdapter {
public:
  LegacyCompatibilityAdapter();
  ~LegacyCompatibilityAdapter();

  LegacyCompatibilityAdapter(const LegacyCompatibilityAdapter &) = delete;
  LegacyCompatibilityAdapter &
  operator=(const LegacyCompatibilityAdapter &) = delete;
  LegacyCompatibilityAdapter(LegacyCompatibilityAdapter &&) noexcept;
  LegacyCompatibilityAdapter &operator=(LegacyCompatibilityAdapter &&) noexcept;

  [[nodiscard]] Result<void> replace(bool enabled,
                                     std::span<const LegacyElement> elements,
                                     std::uint64_t generation,
                                     std::uint64_t transitionAnchorMs);

  [[nodiscard]] Result<LegacyCompatibilitySnapshot>
  snapshot(std::span<const WindowSnapshot> windows) const;

  void clear() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

inline constexpr std::string_view LEGACY_COMPATIBILITY_OWNER =
    "client:compatibility-v1:reserved";

} // namespace hfg::v2
