#include "v2/model/Config.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <set>
#include <utility>

namespace hfg::v2 {
namespace {

template <typename T>
Result<T> invalid(ErrorCode code, std::string path, std::string message) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool validIdentifier(std::string_view value) {
    if (value.empty() || value.size() > Limits::MAX_IDENTIFIER_BYTES || value.starts_with("_hfg_"))
        return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}

Result<MatchExpression> validateMatch(MatchInput input, std::string path) {
    if (input.value.empty())
        return invalid<MatchExpression>(ErrorCode::InvalidRequest, std::move(path), "match value must not be empty");

    if (input.mode == MatchMode::Exact) {
        if (input.value.size() > Limits::MAX_REGEX_BYTES)
            return invalid<MatchExpression>(ErrorCode::InvalidRequest, std::move(path), "exact match exceeds 256 bytes");
        return Result<MatchExpression>::success({
            .mode = MatchMode::Exact,
            .value = std::move(input.value),
            .compiled = nullptr,
        });
    }

    if (input.value.size() > Limits::MAX_REGEX_BYTES)
        return invalid<MatchExpression>(ErrorCode::InvalidRequest, std::move(path), "regular expression exceeds 256 bytes");
    try {
        auto compiled = std::make_shared<const std::regex>(input.value);
        return Result<MatchExpression>::success({
            .mode = MatchMode::Regex,
            .value = std::move(input.value),
            .compiled = std::move(compiled),
        });
    } catch (const std::regex_error&) {
        return invalid<MatchExpression>(ErrorCode::InvalidRequest, std::move(path), "invalid regular expression");
    }
}

bool expressionMatches(const MatchExpression& expression, std::string_view value) {
    if (expression.mode == MatchMode::Exact)
        return value == expression.value;
    if (!expression.compiled)
        return false;
    return std::regex_search(value.begin(), value.end(), *expression.compiled);
}

} // namespace

bool WindowRule::matches(const WindowMetadata& window) const {
    return enabled &&
        (!initialClass || expressionMatches(*initialClass, window.initialClass)) &&
        (!currentClass || expressionMatches(*currentClass, window.currentClass)) &&
        (!initialTitle || expressionMatches(*initialTitle, window.initialTitle)) &&
        (!currentTitle || expressionMatches(*currentTitle, window.currentTitle));
}

bool LayerRule::matches(std::string_view layerNamespace) const {
    return enabled && expressionMatches(namespaceMatch, layerNamespace);
}

Result<ConfigSnapshot> validateConfig(ConfigSnapshotInput input) {
    if (input.version != 2U)
        return invalid<ConfigSnapshot>(ErrorCode::UnsupportedVersion, "version", "configuration version must be 2");
    if (input.materials.empty())
        return invalid<ConfigSnapshot>(ErrorCode::InvalidMaterial, "materials", "at least one material is required");
    if (input.materials.size() > Limits::MAX_MATERIALS_PER_OWNER)
        return invalid<ConfigSnapshot>(ErrorCode::ResourceLimited, "materials", "material limit exceeded");
    if (!input.materials.contains(input.defaultMaterial))
        return invalid<ConfigSnapshot>(ErrorCode::InvalidMaterial, "default_material", "default material was not found");
    if (input.windowRules.size() > Limits::MAX_RULES_PER_KIND)
        return invalid<ConfigSnapshot>(ErrorCode::ResourceLimited, "window_rules", "window rule limit exceeded");
    if (input.layerRules.size() > Limits::MAX_RULES_PER_KIND)
        return invalid<ConfigSnapshot>(ErrorCode::ResourceLimited, "layer_rules", "layer rule limit exceeded");

    ConfigSnapshot result{
        .version = 2,
        .enabled = input.enabled,
        .defaultMaterial = std::move(input.defaultMaterial),
        .materials = {},
        .windowRules = {},
        .layerRules = {},
    };
    for (auto& [name, materialInput] : input.materials) {
        auto material = validateMaterial(name, materialInput);
        if (!material) {
            auto error = material.error();
            error.path = "materials." + name + (error.path.empty() ? "" : "." + error.path);
            return Result<ConfigSnapshot>::failure(std::move(error));
        }
        result.materials.emplace(name, std::move(material.value()));
    }

    std::set<std::string> ruleIds;
    for (std::size_t index = 0; index < input.windowRules.size(); ++index) {
        auto& source = input.windowRules[index];
        const auto base = "window_rules[" + std::to_string(index) + "]";
        if (!validIdentifier(source.id))
            return invalid<ConfigSnapshot>(ErrorCode::InvalidRequest, base + ".id", "invalid rule id");
        if (!ruleIds.insert(source.id).second)
            return invalid<ConfigSnapshot>(ErrorCode::InvalidRequest, base + ".id", "rule ids must be unique");
        if (!result.materials.contains(source.material))
            return invalid<ConfigSnapshot>(ErrorCode::InvalidMaterial, base + ".material", "material was not found");
        if (!source.initialClass && !source.currentClass && !source.initialTitle && !source.currentTitle)
            return invalid<ConfigSnapshot>(ErrorCode::InvalidRequest, base + ".match", "window rule requires at least one match field");

        WindowRule rule{
            .id = std::move(source.id),
            .initialClass = std::nullopt,
            .currentClass = std::nullopt,
            .initialTitle = std::nullopt,
            .currentTitle = std::nullopt,
            .material = std::move(source.material),
            .enabled = source.enabled,
        };
        const auto setMatch = [&](std::optional<MatchInput>& match, std::optional<MatchExpression>& destination, std::string field) -> std::optional<Error> {
            if (!match)
                return std::nullopt;
            auto validated = validateMatch(std::move(*match), base + "." + field);
            if (!validated)
                return validated.error();
            destination = std::move(validated.value());
            return std::nullopt;
        };
        if (auto error = setMatch(source.initialClass, rule.initialClass, "match.initial_class"))
            return Result<ConfigSnapshot>::failure(std::move(*error));
        if (auto error = setMatch(source.currentClass, rule.currentClass, "match.class"))
            return Result<ConfigSnapshot>::failure(std::move(*error));
        if (auto error = setMatch(source.initialTitle, rule.initialTitle, "match.initial_title"))
            return Result<ConfigSnapshot>::failure(std::move(*error));
        if (auto error = setMatch(source.currentTitle, rule.currentTitle, "match.title"))
            return Result<ConfigSnapshot>::failure(std::move(*error));
        result.windowRules.push_back(std::move(rule));
    }

    ruleIds.clear();
    for (std::size_t index = 0; index < input.layerRules.size(); ++index) {
        auto& source = input.layerRules[index];
        const auto base = "layer_rules[" + std::to_string(index) + "]";
        if (!validIdentifier(source.id))
            return invalid<ConfigSnapshot>(ErrorCode::InvalidRequest, base + ".id", "invalid rule id");
        if (!ruleIds.insert(source.id).second)
            return invalid<ConfigSnapshot>(ErrorCode::InvalidRequest, base + ".id", "rule ids must be unique");
        if (!result.materials.contains(source.material))
            return invalid<ConfigSnapshot>(ErrorCode::InvalidMaterial, base + ".material", "material was not found");
        auto match = validateMatch(std::move(source.namespaceMatch), base + ".match.namespace");
        if (!match)
            return Result<ConfigSnapshot>::failure(match.error());
        result.layerRules.push_back({
            .id = std::move(source.id),
            .namespaceMatch = std::move(match.value()),
            .material = std::move(source.material),
            .enabled = source.enabled,
        });
    }

    return Result<ConfigSnapshot>::success(std::move(result));
}

void ConfigStore::beginReload() {
    m_pending.reset();
    m_pendingError.reset();
}

Result<void> ConfigStore::stage(ConfigSnapshotInput input) {
    auto result = validateConfig(std::move(input));
    if (!result) {
        m_pending.reset();
        m_pendingError = result.error();
        return Result<void>::failure(*m_pendingError);
    }
    m_pending = std::move(result.value());
    m_pendingError.reset();
    return Result<void>::success();
}

Result<std::uint64_t> ConfigStore::commitReload() {
    if (!m_pending) {
        if (!m_pendingError)
            m_pendingError = Error{
                .code = ErrorCode::InvalidRequest,
                .path = "config",
                .message = "reload did not provide a valid v2 configuration",
            };
        return Result<std::uint64_t>::failure(*m_pendingError);
    }
    m_active = std::move(m_pending);
    m_pending.reset();
    m_pendingError.reset();
    ++m_generation;
    return Result<std::uint64_t>::success(m_generation);
}

const ConfigSnapshot* ConfigStore::active() const noexcept {
    return m_active ? &*m_active : nullptr;
}

std::uint64_t ConfigStore::generation() const noexcept {
    return m_generation;
}

const std::optional<Error>& ConfigStore::pendingError() const noexcept {
    return m_pendingError;
}

} // namespace hfg::v2
