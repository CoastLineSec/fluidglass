#include "v2/compat/LegacyCompatibility.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <utility>

namespace hfg::v2 {
namespace {

enum class WindowMatchField {
  Address,
  Title,
  Class,
  ClassOrTitle,
};

struct WindowMatch {
  WindowMatchField field = WindowMatchField::ClassOrTitle;
  std::string value;
  std::optional<std::regex> expression;
};

struct PreparedElement {
  std::string sourceId;
  Target target;
  std::optional<WindowMatch> windowMatch;
  Rect legacyWindowGeometry;
  bool automaticWindowGeometry = false;
};

template <typename T>
Result<T> failure(ErrorCode code, std::string path, std::string message) {
  return Result<T>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

std::uint64_t fnv1a(std::string_view value) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : value) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string targetIdFor(std::string_view sourceId) {
  return std::format("v1-{:016x}", fnv1a(sourceId));
}

bool sameMaterialValues(const Material &left, const Material &right) {
  auto normalizedLeft = left;
  auto normalizedRight = right;
  normalizedLeft.name.clear();
  normalizedRight.name.clear();
  return normalizedLeft == normalizedRight;
}

Result<WindowMatch> compileWindowMatch(std::string selector) {
  if (selector.empty() || selector.size() > Limits::MAX_REGEX_BYTES)
    return failure<WindowMatch>(
        ErrorCode::InvalidTarget, "selector",
        "legacy window selector must be non-empty and bounded");

  WindowMatch result;
  if (selector.starts_with("address:")) {
    result.field = WindowMatchField::Address;
    selector.erase(0, 8);
    if (selector.starts_with("0x") || selector.starts_with("0X"))
      selector.erase(0, 2);
    if (selector.empty() || selector.size() > 2U * sizeof(std::uintptr_t) ||
        !std::ranges::all_of(selector, [](const unsigned char character) {
          return std::isxdigit(character);
        }))
      return failure<WindowMatch>(
          ErrorCode::InvalidTarget, "selector",
          "legacy address selector must contain a hexadecimal window address");
    std::ranges::transform(selector, selector.begin(),
                           [](const unsigned char character) {
                             return static_cast<char>(std::tolower(character));
                           });
    result.value = "0x" + selector;
    return Result<WindowMatch>::success(std::move(result));
  }

  if (selector.starts_with("title:")) {
    result.field = WindowMatchField::Title;
    selector.erase(0, 6);
  } else if (selector.starts_with("class:")) {
    result.field = WindowMatchField::Class;
    selector.erase(0, 6);
  }
  if (selector.empty())
    return failure<WindowMatch>(ErrorCode::InvalidTarget, "selector",
                                "legacy window expression must not be empty");
  try {
    result.expression.emplace(selector);
  } catch (const std::regex_error &) {
    return failure<WindowMatch>(ErrorCode::InvalidTarget, "selector",
                                "legacy window expression is invalid");
  }
  result.value = std::move(selector);
  return Result<WindowMatch>::success(std::move(result));
}

bool eligibleWindow(const WindowSnapshot &window) {
  return window.mapped && !window.fadingOut && !window.readyToDelete;
}

bool matches(const WindowSnapshot &window, const WindowMatch &match) {
  if (!eligibleWindow(window))
    return false;
  if (match.field == WindowMatchField::Address)
    return window.address == match.value;
  if (!match.expression)
    return false;
  switch (match.field) {
  case WindowMatchField::Title:
    return std::regex_search(window.currentTitle, *match.expression);
  case WindowMatchField::Class:
    return std::regex_search(window.currentClass, *match.expression);
  case WindowMatchField::ClassOrTitle:
    return std::regex_search(window.currentClass, *match.expression) ||
           std::regex_search(window.currentTitle, *match.expression);
  case WindowMatchField::Address:
    return false;
  }
  return false;
}

bool nearlyEqual(double left, double right) {
  return std::isfinite(left) && std::isfinite(right) &&
         std::abs(left - right) <=
             std::max(0.5, 0.001 * std::max(std::abs(left), std::abs(right)));
}

bool wholeWindowGeometry(const PreparedElement &element,
                         const WindowSnapshot &window) {
  if (element.automaticWindowGeometry)
    return true;
  return nearlyEqual(element.legacyWindowGeometry.x, 0.0) &&
         nearlyEqual(element.legacyWindowGeometry.y, 0.0) &&
         nearlyEqual(element.legacyWindowGeometry.width,
                     window.globalGeometry.width) &&
         nearlyEqual(element.legacyWindowGeometry.height,
                     window.globalGeometry.height);
}

void applyAutomaticWindowRounding(Shape &shape, double rounding) {
  if (!std::isfinite(rounding) || rounding < 0.0)
    return;
  if (auto *rounded = std::get_if<RoundedRectShape>(&shape)) {
    rounded->radius = rounding;
    return;
  }
  auto *compound = std::get_if<CompoundShape>(&shape);
  if (!compound || !compound->base)
    return;
  compound->base->corners = {
      .topLeft = rounding,
      .topRight = rounding,
      .bottomRight = rounding,
      .bottomLeft = rounding,
  };
}

Result<Target> prepareTarget(const LegacyElement &element, std::string targetId,
                             std::string materialName) {
  TargetInput input{};
  input.id = std::move(targetId);
  input.kind = TargetKind::Region;
  input.material = MaterialReference{
      .source = MaterialSource::Session,
      .name = std::move(materialName),
  };
  input.shape = element.shape;
  input.selector = RegionSelector{.output = element.output};
  input.geometry = element.geometry;
  input.stage =
      element.under ? RenderStage::PostWallpaper : RenderStage::PostWindows;
  input.transition = element.transition;
  input.enabled = element.enabled;

  switch (element.attachment) {
  case LegacyAttachmentKind::Region:
    break;
  case LegacyAttachmentKind::Layer:
    input.kind = TargetKind::Layer;
    input.selector = LayerSelector{.namespaceName = element.selector};
    input.stage.reset();
    if (element.geometry.width <= 0.5 || element.geometry.height <= 0.5)
      input.geometry.reset();
    break;
  case LegacyAttachmentKind::Window:
    if (!element.under)
      return failure<Target>(ErrorCode::UnsupportedTarget, "under",
                             "v2 compatibility supports legacy window glass "
                             "only below its window");
    input.kind = TargetKind::Window;
    input.selector = WindowSelector{
        .address = "0x1",
        .pid = 1,
        .initialClass = std::nullopt,
    };
    input.geometry.reset();
    input.stage.reset();
    break;
  }
  return validateTarget(std::move(input));
}

} // namespace

struct LegacyCompatibilityAdapter::Impl {
  bool enabled = false;
  std::uint64_t generation = 0;
  std::uint64_t transitionAnchorMs = 0;
  std::map<std::string, Material> materials;
  std::vector<PreparedElement> elements;
};

LegacyCompatibilityAdapter::LegacyCompatibilityAdapter()
    : m_impl(std::make_unique<Impl>()) {}

LegacyCompatibilityAdapter::~LegacyCompatibilityAdapter() = default;

LegacyCompatibilityAdapter::LegacyCompatibilityAdapter(
    LegacyCompatibilityAdapter &&) noexcept = default;

LegacyCompatibilityAdapter &LegacyCompatibilityAdapter::operator=(
    LegacyCompatibilityAdapter &&) noexcept = default;

Result<void> LegacyCompatibilityAdapter::replace(
    bool enabled, std::span<const LegacyElement> elements,
    std::uint64_t generation, std::uint64_t transitionAnchorMs) {
  if (elements.size() > Limits::MAX_TARGETS_PER_SESSION)
    return failure<void>(
        ErrorCode::ResourceLimited, "elements",
        "legacy element count exceeds the v2 compatibility target limit");
  if (generation == 0U)
    return failure<void>(ErrorCode::InvalidRequest, "generation",
                         "legacy compatibility generation must not be zero");

  auto next = std::make_unique<Impl>();
  next->enabled = enabled;
  next->generation = generation;
  next->transitionAnchorMs = transitionAnchorMs;
  next->elements.reserve(elements.size());

  std::set<std::string> sourceIds;
  std::set<std::string> targetIds;
  for (std::size_t index = 0; index < elements.size(); ++index) {
    const auto &element = elements[index];
    const auto path = "elements[" + std::to_string(index) + "]";
    if (element.sourceId.empty() ||
        element.sourceId.size() > Limits::MAX_IDENTIFIER_BYTES)
      return failure<void>(ErrorCode::InvalidTarget, path + ".id",
                           "legacy source id must be non-empty and bounded");
    if (!sourceIds.insert(element.sourceId).second)
      return failure<void>(ErrorCode::InvalidTarget, path + ".id",
                           "legacy source ids must be unique");

    const auto targetId = targetIdFor(element.sourceId);
    if (!targetIds.insert(targetId).second)
      return failure<void>(
          ErrorCode::InvalidTarget, path + ".id",
          "legacy source ids collide after compatibility encoding");

    std::string materialName;
    auto candidate =
        validateMaterial(std::format("v1-material-{}", next->materials.size()),
                         element.material);
    if (!candidate)
      return Result<void>::failure({
          .code = candidate.error().code,
          .path = path + ".material." + candidate.error().path,
          .message = candidate.error().message,
      });
    const auto existing =
        std::ranges::find_if(next->materials, [&](const auto &entry) {
          return sameMaterialValues(entry.second, candidate.value());
        });
    if (existing != next->materials.end()) {
      materialName = existing->first;
    } else {
      if (next->materials.size() >= Limits::MAX_MATERIALS_PER_OWNER)
        return failure<void>(
            ErrorCode::ResourceLimited, path + ".material",
            "legacy elements exceed the v2 compatibility material limit");
      materialName = candidate.value().name;
      next->materials.emplace(materialName, std::move(candidate.value()));
    }

    auto target = prepareTarget(element, targetId, materialName);
    if (!target)
      return Result<void>::failure({
          .code = target.error().code,
          .path = path + "." + target.error().path,
          .message = target.error().message,
      });

    PreparedElement prepared{
        .sourceId = element.sourceId,
        .target = std::move(target.value()),
        .windowMatch = std::nullopt,
        .legacyWindowGeometry = element.geometry,
        .automaticWindowGeometry = element.automaticWindowGeometry,
    };
    if (element.attachment == LegacyAttachmentKind::Window) {
      auto match = compileWindowMatch(element.selector);
      if (!match)
        return Result<void>::failure({
            .code = match.error().code,
            .path = path + "." + match.error().path,
            .message = match.error().message,
        });
      prepared.windowMatch = std::move(match.value());
    }
    next->elements.push_back(std::move(prepared));
  }

  m_impl = std::move(next);
  return Result<void>::success();
}

Result<LegacyCompatibilitySnapshot> LegacyCompatibilityAdapter::snapshot(
    std::span<const WindowSnapshot> windows) const {
  if (!m_impl)
    return failure<LegacyCompatibilitySnapshot>(
        ErrorCode::InternalError, "compatibility",
        "legacy compatibility state is unavailable");

  LegacyCompatibilitySnapshot result{
      .session =
          {
              .owner = std::string(LEGACY_COMPATIBILITY_OWNER),
              .clientId = "compatibility-v1",
              .mode = SessionMode::Client,
              .generation = m_impl->generation,
              .expiresAtMs = std::numeric_limits<std::uint64_t>::max(),
              .transitionAnchorMs = m_impl->transitionAnchorMs,
              .materials = m_impl->enabled ? m_impl->materials
                                           : std::map<std::string, Material>{},
              .targets = {},
          },
      .identities = {},
  };
  if (!m_impl->enabled)
    return Result<LegacyCompatibilitySnapshot>::success(std::move(result));

  result.session.targets.reserve(m_impl->elements.size());
  result.identities.reserve(m_impl->elements.size());
  for (std::size_t index = 0; index < m_impl->elements.size(); ++index) {
    const auto &element = m_impl->elements[index];
    auto target = element.target;
    if (element.windowMatch) {
      const WindowSnapshot *matched = nullptr;
      for (const auto &window : windows) {
        if (!matches(window, *element.windowMatch))
          continue;
        if (matched)
          return failure<LegacyCompatibilitySnapshot>(
              ErrorCode::UnresolvedTarget,
              "elements[" + std::to_string(index) + "].selector",
              "legacy window selector resolves to more than one mapped window");
        matched = &window;
      }
      if (!matched)
        return failure<LegacyCompatibilitySnapshot>(
            ErrorCode::UnresolvedTarget,
            "elements[" + std::to_string(index) + "].selector",
            "legacy window selector has no mapped match");
      if (!wholeWindowGeometry(element, *matched))
        return failure<LegacyCompatibilitySnapshot>(
            ErrorCode::UnsupportedTarget,
            "elements[" + std::to_string(index) + "].geometry",
            "v2 compatibility cannot represent a legacy window subregion");

      auto *selector = std::get_if<WindowSelector>(&target.selector);
      if (!selector)
        return failure<LegacyCompatibilitySnapshot>(
            ErrorCode::InternalError,
            "elements[" + std::to_string(index) + "].selector",
            "prepared compatibility target lost its window selector");
      selector->address = matched->address;
      selector->pid = matched->pid > 0
                          ? std::optional<std::int64_t>{matched->pid}
                          : std::nullopt;
      selector->initialClass =
          !matched->initialClass.empty()
              ? std::optional<std::string>{matched->initialClass}
              : std::nullopt;
      if (element.automaticWindowGeometry)
        applyAutomaticWindowRounding(target.shape, matched->rounding);
      TargetInput validatedInput{
          .id = target.id,
          .kind = target.kind,
          .material = target.material,
          .shape = target.shape,
          .selector = target.selector,
          .geometry = target.geometry,
          .stage = target.stage,
          .transition = target.transition,
          .enabled = target.enabled,
      };
      auto validated = validateTarget(std::move(validatedInput));
      if (!validated)
        return Result<LegacyCompatibilitySnapshot>::failure({
            .code = validated.error().code,
            .path = "elements[" + std::to_string(index) + "]." +
                    validated.error().path,
            .message = validated.error().message,
        });
      target = std::move(validated.value());
    }
    result.identities.push_back({
        .sourceId = element.sourceId,
        .targetId = target.id,
    });
    result.session.targets.push_back(std::move(target));
  }
  return Result<LegacyCompatibilitySnapshot>::success(std::move(result));
}

void LegacyCompatibilityAdapter::clear() noexcept {
  if (!m_impl)
    return;
  m_impl->enabled = false;
  m_impl->generation = 0;
  m_impl->transitionAnchorMs = 0;
  m_impl->materials.clear();
  m_impl->elements.clear();
}

} // namespace hfg::v2
