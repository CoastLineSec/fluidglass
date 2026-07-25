#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Material.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace hfg::v2 {

enum class MatchMode {
    Exact,
    Regex,
};

struct MatchInput {
    MatchMode   mode = MatchMode::Exact;
    std::string value;
};

struct MatchExpression {
    MatchMode                         mode = MatchMode::Exact;
    std::string                       value;
    std::shared_ptr<const std::regex> compiled;
};

struct WindowRuleInput {
    std::string              id;
    std::optional<MatchInput> initialClass;
    std::optional<MatchInput> currentClass;
    std::optional<MatchInput> initialTitle;
    std::optional<MatchInput> currentTitle;
    std::string              material;
    bool                     enabled = true;
};

struct LayerRuleInput {
    std::string id;
    MatchInput  namespaceMatch;
    std::string material;
    bool        enabled = true;
};

struct ConfigSnapshotInput {
    std::uint64_t                       version = 2;
    bool                                enabled = true;
    std::string                         defaultMaterial;
    std::map<std::string, MaterialInput> materials;
    std::vector<WindowRuleInput>         windowRules;
    std::vector<LayerRuleInput>          layerRules;
};

struct WindowMetadata {
    std::string_view initialClass;
    std::string_view currentClass;
    std::string_view initialTitle;
    std::string_view currentTitle;
};

struct WindowRule {
    std::string                    id;
    std::optional<MatchExpression> initialClass;
    std::optional<MatchExpression> currentClass;
    std::optional<MatchExpression> initialTitle;
    std::optional<MatchExpression> currentTitle;
    std::string                    material;
    bool                           enabled = true;

    [[nodiscard]] bool matches(const WindowMetadata& window) const;
};

struct LayerRule {
    std::string     id;
    MatchExpression namespaceMatch;
    std::string     material;
    bool            enabled = true;

    [[nodiscard]] bool matches(std::string_view layerNamespace) const;
};

struct ConfigSnapshot {
    std::uint64_t                   version = 2;
    bool                            enabled = true;
    std::string                     defaultMaterial;
    std::map<std::string, Material> materials;
    std::vector<WindowRule>         windowRules;
    std::vector<LayerRule>          layerRules;
};

[[nodiscard]] Result<ConfigSnapshot> validateConfig(ConfigSnapshotInput input);

class ConfigStore {
  public:
    void beginReload();
    [[nodiscard]] Result<void> stage(ConfigSnapshotInput input);
    [[nodiscard]] Result<std::uint64_t> commitReload();

    [[nodiscard]] const ConfigSnapshot* active() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] const std::optional<Error>& pendingError() const noexcept;

  private:
    std::optional<ConfigSnapshot> m_active;
    std::optional<ConfigSnapshot> m_pending;
    std::optional<Error>          m_pendingError;
    std::uint64_t                 m_generation = 0;
};

} // namespace hfg::v2
