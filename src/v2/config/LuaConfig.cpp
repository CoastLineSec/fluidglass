#include "v2/config/LuaConfig.hpp"

#include "v2/core/Limits.hpp"

#include <lua.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace hfg::v2 {
namespace {

template <typename T>
Result<T> invalid(std::string path, std::string message) {
    return Result<T>::failure({
        .code = ErrorCode::InvalidRequest,
        .path = std::move(path),
        .message = std::move(message),
    });
}

void pushField(lua_State* state, int table, std::string_view field) {
    table = lua_absindex(state, table);
    lua_pushlstring(state, field.data(), field.size());
    lua_rawget(state, table);
}

bool validMaterialName(std::string_view value) {
    if (value.empty() || value.size() > Limits::MAX_IDENTIFIER_BYTES || value.starts_with("_hfg_"))
        return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}

std::optional<Error> rejectUnknown(
    lua_State* state,
    int index,
    const std::set<std::string_view>& allowed,
    std::string_view path) {
    index = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            return Error{
                .code = ErrorCode::InvalidRequest,
                .path = std::string(path),
                .message = "table keys must be strings",
            };
        }
        std::size_t length = 0;
        const char* keyData = lua_tolstring(state, -2, &length);
        const std::string_view key(keyData, length);
        if (!allowed.contains(key)) {
            lua_pop(state, 2);
            return Error{
                .code = ErrorCode::InvalidRequest,
                .path = path.empty() ? std::string(key) : std::string(path) + "." + std::string(key),
                .message = "unknown field",
            };
        }
        lua_pop(state, 1);
    }
    return std::nullopt;
}

Result<std::string> requiredString(
    lua_State* state,
    int table,
    std::string_view field,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pop(state, 1);
        return invalid<std::string>(std::move(path), "expected a string");
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    std::string result(value, length);
    lua_pop(state, 1);
    return Result<std::string>::success(std::move(result));
}

Result<bool> requiredBoolean(
    lua_State* state,
    int table,
    std::string_view field,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
        lua_pop(state, 1);
        return invalid<bool>(std::move(path), "expected a boolean");
    }
    const bool result = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return Result<bool>::success(result);
}

Result<std::uint64_t> requiredUnsigned(
    lua_State* state,
    int table,
    std::string_view field,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 1);
        return invalid<std::uint64_t>(std::move(path), "expected an integer");
    }
    int valid = 0;
    const lua_Integer value = lua_tointegerx(state, -1, &valid);
    lua_pop(state, 1);
    if (!valid || value < 0)
        return invalid<std::uint64_t>(std::move(path), "expected a non-negative integer");
    return Result<std::uint64_t>::success(static_cast<std::uint64_t>(value));
}

std::optional<Error> optionalBoolean(
    lua_State* state,
    int table,
    std::string_view field,
    bool& destination,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
        lua_pop(state, 1);
        return Error{ErrorCode::InvalidRequest, std::move(path), "expected a boolean"};
    }
    destination = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return std::nullopt;
}

std::optional<Error> optionalNumber(
    lua_State* state,
    int table,
    std::string_view field,
    double& destination,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    if (lua_type(state, -1) != LUA_TNUMBER) {
        lua_pop(state, 1);
        return Error{ErrorCode::InvalidRequest, std::move(path), "expected a number"};
    }
    int valid = 0;
    const double value = lua_tonumberx(state, -1, &valid);
    lua_pop(state, 1);
    if (!valid || !std::isfinite(value))
        return Error{ErrorCode::InvalidRequest, std::move(path), "expected a finite number"};
    destination = value;
    return std::nullopt;
}

std::optional<Error> optionalNumber(
    lua_State* state,
    int table,
    std::string_view field,
    std::optional<double>& destination,
    std::string path) {
    double value = 0.0;
    table = lua_absindex(state, table);
    pushField(state, table, field);
    const bool absent = lua_isnil(state, -1);
    lua_pop(state, 1);
    if (absent)
        return std::nullopt;
    if (auto error = optionalNumber(state, table, field, value, std::move(path)))
        return error;
    destination = value;
    return std::nullopt;
}

std::optional<Error> optionalString(
    lua_State* state,
    int table,
    std::string_view field,
    std::string& destination,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return std::nullopt;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pop(state, 1);
        return Error{ErrorCode::InvalidRequest, std::move(path), "expected a string"};
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    destination.assign(value, length);
    lua_pop(state, 1);
    return std::nullopt;
}

Result<std::size_t> arrayLength(
    lua_State* state,
    int index,
    std::string path,
    std::size_t limit) {
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<std::size_t>(std::move(path), "expected an array table");

    std::size_t count = 0;
    std::size_t maximum = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (!lua_isinteger(state, -2)) {
            lua_pop(state, 2);
            return invalid<std::size_t>(std::move(path), "array keys must be positive integers");
        }
        int valid = 0;
        const lua_Integer key = lua_tointegerx(state, -2, &valid);
        if (!valid || key <= 0) {
            lua_pop(state, 2);
            return invalid<std::size_t>(std::move(path), "array keys must be positive integers");
        }
        ++count;
        maximum = std::max(maximum, static_cast<std::size_t>(key));
        if (count > limit || maximum > limit) {
            lua_pop(state, 2);
            return invalid<std::size_t>(std::move(path), "array limit exceeded");
        }
        lua_pop(state, 1);
    }
    if (maximum != count)
        return invalid<std::size_t>(std::move(path), "array must be contiguous from index 1");
    return Result<std::size_t>::success(count);
}

Result<MatchInput> parseMatch(lua_State* state, int index, std::string path) {
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<MatchInput>(std::move(path), "match expression must be a table");
    static const std::set<std::string_view> fields{"exact", "regex"};
    if (auto error = rejectUnknown(state, index, fields, path))
        return Result<MatchInput>::failure(std::move(*error));

    pushField(state, index, "exact");
    const bool hasExact = !lua_isnil(state, -1);
    lua_pop(state, 1);
    pushField(state, index, "regex");
    const bool hasRegex = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (hasExact == hasRegex)
        return invalid<MatchInput>(std::move(path), "match expression requires exactly one of exact or regex");

    const auto field = hasExact ? std::string_view("exact") : std::string_view("regex");
    auto value = requiredString(state, index, field, path + "." + std::string(field));
    if (!value)
        return Result<MatchInput>::failure(value.error());
    return Result<MatchInput>::success({
        .mode = hasExact ? MatchMode::Exact : MatchMode::Regex,
        .value = std::move(value.value()),
    });
}

Result<MaterialInput> parseMaterial(lua_State* state, int index, std::string path) {
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<MaterialInput>(std::move(path), "material must be a table");
    static const std::set<std::string_view> fields{
        "glass_level", "blur_level", "tint_level", "tint_enabled", "tint_color", "light_mode",
        "refraction", "rim_band", "bevel", "rim_width", "highlight", "shadow", "light_angle",
        "specular", "chroma", "edge_depth", "lens", "lens_band", "gloss",
    };
    if (auto error = rejectUnknown(state, index, fields, path))
        return Result<MaterialInput>::failure(std::move(*error));

    MaterialInput input;
    if (auto error = optionalNumber(state, index, "glass_level", input.glassLevel, path + ".glass_level")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "blur_level", input.blurLevel, path + ".blur_level")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "tint_level", input.tintLevel, path + ".tint_level")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalBoolean(state, index, "tint_enabled", input.tintEnabled, path + ".tint_enabled")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalString(state, index, "tint_color", input.tintColor, path + ".tint_color")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalBoolean(state, index, "light_mode", input.lightMode, path + ".light_mode")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "refraction", input.refraction, path + ".refraction")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "rim_band", input.rimBand, path + ".rim_band")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "bevel", input.bevel, path + ".bevel")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "rim_width", input.rimWidth, path + ".rim_width")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "highlight", input.highlight, path + ".highlight")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "shadow", input.shadow, path + ".shadow")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "light_angle", input.lightAngle, path + ".light_angle")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "specular", input.specular, path + ".specular")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "chroma", input.chroma, path + ".chroma")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "edge_depth", input.edgeDepth, path + ".edge_depth")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "lens", input.lens, path + ".lens")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "lens_band", input.lensBand, path + ".lens_band")) return Result<MaterialInput>::failure(std::move(*error));
    if (auto error = optionalNumber(state, index, "gloss", input.gloss, path + ".gloss")) return Result<MaterialInput>::failure(std::move(*error));
    return Result<MaterialInput>::success(std::move(input));
}

Result<std::optional<MatchInput>> optionalMatch(
    lua_State* state,
    int table,
    std::string_view field,
    std::string path) {
    table = lua_absindex(state, table);
    pushField(state, table, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return Result<std::optional<MatchInput>>::success(std::nullopt);
    }
    auto match = parseMatch(state, -1, std::move(path));
    lua_pop(state, 1);
    if (!match)
        return Result<std::optional<MatchInput>>::failure(match.error());
    return Result<std::optional<MatchInput>>::success(std::move(match.value()));
}

Result<WindowRuleInput> parseWindowRule(lua_State* state, int index, std::string path) {
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<WindowRuleInput>(std::move(path), "window rule must be a table");
    static const std::set<std::string_view> fields{"id", "match", "material", "enabled"};
    if (auto error = rejectUnknown(state, index, fields, path))
        return Result<WindowRuleInput>::failure(std::move(*error));

    auto id = requiredString(state, index, "id", path + ".id");
    if (!id) return Result<WindowRuleInput>::failure(id.error());
    std::string material;
    if (auto error = optionalString(
            state, index, "material", material, path + ".material"))
        return Result<WindowRuleInput>::failure(std::move(*error));

    bool enabled = true;
    if (auto error = optionalBoolean(state, index, "enabled", enabled, path + ".enabled"))
        return Result<WindowRuleInput>::failure(std::move(*error));

    pushField(state, index, "match");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return invalid<WindowRuleInput>(path + ".match", "match must be a table");
    }
    const int matchTable = lua_absindex(state, -1);
    static const std::set<std::string_view> matchFields{"initial_class", "class", "initial_title", "title"};
    if (auto error = rejectUnknown(state, matchTable, matchFields, path + ".match")) {
        lua_pop(state, 1);
        return Result<WindowRuleInput>::failure(std::move(*error));
    }
    auto initialClass = optionalMatch(state, matchTable, "initial_class", path + ".match.initial_class");
    auto currentClass = optionalMatch(state, matchTable, "class", path + ".match.class");
    auto initialTitle = optionalMatch(state, matchTable, "initial_title", path + ".match.initial_title");
    auto currentTitle = optionalMatch(state, matchTable, "title", path + ".match.title");
    lua_pop(state, 1);
    if (!initialClass) return Result<WindowRuleInput>::failure(initialClass.error());
    if (!currentClass) return Result<WindowRuleInput>::failure(currentClass.error());
    if (!initialTitle) return Result<WindowRuleInput>::failure(initialTitle.error());
    if (!currentTitle) return Result<WindowRuleInput>::failure(currentTitle.error());

    return Result<WindowRuleInput>::success({
        .id = std::move(id.value()),
        .initialClass = std::move(initialClass.value()),
        .currentClass = std::move(currentClass.value()),
        .initialTitle = std::move(initialTitle.value()),
        .currentTitle = std::move(currentTitle.value()),
        .material = std::move(material),
        .enabled = enabled,
    });
}

Result<LayerRuleInput> parseLayerRule(lua_State* state, int index, std::string path) {
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<LayerRuleInput>(std::move(path), "layer rule must be a table");
    static const std::set<std::string_view> fields{"id", "match", "material", "enabled"};
    if (auto error = rejectUnknown(state, index, fields, path))
        return Result<LayerRuleInput>::failure(std::move(*error));

    auto id = requiredString(state, index, "id", path + ".id");
    if (!id) return Result<LayerRuleInput>::failure(id.error());
    std::string material;
    if (auto error = optionalString(
            state, index, "material", material, path + ".material"))
        return Result<LayerRuleInput>::failure(std::move(*error));
    bool enabled = true;
    if (auto error = optionalBoolean(state, index, "enabled", enabled, path + ".enabled"))
        return Result<LayerRuleInput>::failure(std::move(*error));

    pushField(state, index, "match");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return invalid<LayerRuleInput>(path + ".match", "match must be a table");
    }
    const int matchTable = lua_absindex(state, -1);
    static const std::set<std::string_view> matchFields{"namespace"};
    if (auto error = rejectUnknown(state, matchTable, matchFields, path + ".match")) {
        lua_pop(state, 1);
        return Result<LayerRuleInput>::failure(std::move(*error));
    }
    pushField(state, matchTable, "namespace");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 2);
        return invalid<LayerRuleInput>(path + ".match.namespace", "namespace match is required");
    }
    auto namespaceMatch = parseMatch(state, -1, path + ".match.namespace");
    lua_pop(state, 2);
    if (!namespaceMatch)
        return Result<LayerRuleInput>::failure(namespaceMatch.error());
    return Result<LayerRuleInput>::success({
        .id = std::move(id.value()),
        .namespaceMatch = std::move(namespaceMatch.value()),
        .material = std::move(material),
        .enabled = enabled,
    });
}

} // namespace

Result<ConfigSnapshotInput> parseLuaConfig(lua_State* state, int index) {
    if (!state)
        return invalid<ConfigSnapshotInput>("config", "Lua state is unavailable");
    index = lua_absindex(state, index);
    if (lua_type(state, index) != LUA_TTABLE)
        return invalid<ConfigSnapshotInput>("config", "configuration must be a table");
    static const std::set<std::string_view> fields{
        "version", "enabled", "default_material", "materials", "window_rules", "layer_rules",
    };
    if (auto error = rejectUnknown(state, index, fields, ""))
        return Result<ConfigSnapshotInput>::failure(std::move(*error));

    auto version = requiredUnsigned(state, index, "version", "version");
    auto enabled = requiredBoolean(state, index, "enabled", "enabled");
    auto defaultMaterial = requiredString(state, index, "default_material", "default_material");
    if (!version) return Result<ConfigSnapshotInput>::failure(version.error());
    if (!enabled) return Result<ConfigSnapshotInput>::failure(enabled.error());
    if (!defaultMaterial) return Result<ConfigSnapshotInput>::failure(defaultMaterial.error());

    ConfigSnapshotInput input{
        .version = version.value(),
        .enabled = enabled.value(),
        .defaultMaterial = std::move(defaultMaterial.value()),
        .materials = {},
        .windowRules = {},
        .layerRules = {},
    };

    pushField(state, index, "materials");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return invalid<ConfigSnapshotInput>("materials", "materials must be a table");
    }
    const int materials = lua_absindex(state, -1);
    lua_pushnil(state);
    while (lua_next(state, materials) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 3);
            return invalid<ConfigSnapshotInput>("materials", "material names must be strings");
        }
        std::size_t length = 0;
        const char* nameData = lua_tolstring(state, -2, &length);
        std::string name(nameData, length);
        if (!validMaterialName(name)) {
            lua_pop(state, 3);
            return invalid<ConfigSnapshotInput>("materials", "invalid material name");
        }
        if (input.materials.size() >= Limits::MAX_MATERIALS_PER_OWNER) {
            lua_pop(state, 3);
            return invalid<ConfigSnapshotInput>("materials", "material limit exceeded");
        }
        auto material = parseMaterial(state, -1, "materials." + name);
        if (!material) {
            lua_pop(state, 3);
            return Result<ConfigSnapshotInput>::failure(material.error());
        }
        input.materials.emplace(std::move(name), std::move(material.value()));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    pushField(state, index, "window_rules");
    auto windowCount = arrayLength(state, -1, "window_rules", Limits::MAX_RULES_PER_KIND);
    if (!windowCount) {
        lua_pop(state, 1);
        return Result<ConfigSnapshotInput>::failure(windowCount.error());
    }
    const int windows = lua_absindex(state, -1);
    for (std::size_t ruleIndex = 1; ruleIndex <= windowCount.value(); ++ruleIndex) {
        lua_rawgeti(state, windows, static_cast<lua_Integer>(ruleIndex));
        auto rule = parseWindowRule(
            state,
            -1,
            "window_rules[" + std::to_string(ruleIndex - 1U) + "]");
        lua_pop(state, 1);
        if (!rule) {
            lua_pop(state, 1);
            return Result<ConfigSnapshotInput>::failure(rule.error());
        }
        input.windowRules.push_back(std::move(rule.value()));
    }
    lua_pop(state, 1);

    pushField(state, index, "layer_rules");
    auto layerCount = arrayLength(state, -1, "layer_rules", Limits::MAX_RULES_PER_KIND);
    if (!layerCount) {
        lua_pop(state, 1);
        return Result<ConfigSnapshotInput>::failure(layerCount.error());
    }
    const int layers = lua_absindex(state, -1);
    for (std::size_t ruleIndex = 1; ruleIndex <= layerCount.value(); ++ruleIndex) {
        lua_rawgeti(state, layers, static_cast<lua_Integer>(ruleIndex));
        auto rule = parseLayerRule(
            state,
            -1,
            "layer_rules[" + std::to_string(ruleIndex - 1U) + "]");
        lua_pop(state, 1);
        if (!rule) {
            lua_pop(state, 1);
            return Result<ConfigSnapshotInput>::failure(rule.error());
        }
        input.layerRules.push_back(std::move(rule.value()));
    }
    lua_pop(state, 1);

    return Result<ConfigSnapshotInput>::success(std::move(input));
}

} // namespace hfg::v2
