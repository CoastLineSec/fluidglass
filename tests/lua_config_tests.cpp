#include "TestHarness.hpp"

#include "v2/config/LuaConfig.hpp"

#include <lua.hpp>

#include <memory>
#include <string_view>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

struct LuaCloser {
    void operator()(lua_State* state) const {
        if (state)
            lua_close(state);
    }
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

LuaState evaluate(std::string_view source) {
    LuaState state(luaL_newstate());
    if (!state)
        throw hfg::test::Failure("could not create Lua state");
    luaL_openlibs(state.get());
    if (luaL_loadbuffer(state.get(), source.data(), source.size(), "test") != LUA_OK)
        throw hfg::test::Failure(lua_tostring(state.get(), -1));
    if (lua_pcall(state.get(), 0, 1, 0) != LUA_OK)
        throw hfg::test::Failure(lua_tostring(state.get(), -1));
    return state;
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"minimal snapshot", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {fluid = {}},
                window_rules = {},
                layer_rules = {},
            })");
            auto parsed = parseLuaConfig(state.get(), -1);
            require(parsed.hasValue(), "minimal snapshot failed");
            auto validated = validateConfig(std::move(parsed.value()));
            require(validated.hasValue(), "minimal snapshot did not validate");
            require(validated.value().materials.contains("fluid"), "material was lost");
        }},
        Case{"complete rules and material", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {
                    fluid = {
                        glass_level = 0.4,
                        blur_level = 0.2,
                        tint_enabled = true,
                        tint_color = "#112233",
                        light_angle = 120,
                    },
                },
                window_rules = {{
                    id = "files",
                    match = {
                        initial_class = {exact = "org.gnome.Nautilus"},
                        title = {regex = "^Files"},
                    },
                    material = "fluid",
                    enabled = false,
                }},
                layer_rules = {{
                    id = "shell",
                    match = {namespace = {regex = "^example:"}},
                    material = "fluid",
                }},
            })");
            auto parsed = parseLuaConfig(state.get(), -1);
            require(parsed.hasValue(), "complete snapshot failed");
            require(parsed.value().windowRules.size() == 1, "window rule was lost");
            require(parsed.value().layerRules.size() == 1, "layer rule was lost");
            auto validated = validateConfig(std::move(parsed.value()));
            require(validated.hasValue(), "complete snapshot did not validate");
            require(validated.value().windowRules[0].enabled == false, "rule enabled state changed");
            require(validated.value().materials.at("fluid").tintEnabled, "material boolean changed");
        }},
        Case{"unknown fields are rejected precisely", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {fluid = {surprise = 1}},
                window_rules = {},
                layer_rules = {},
            })");
            const auto parsed = parseLuaConfig(state.get(), -1);
            require(!parsed && parsed.error().path == "materials.fluid.surprise",
                    "unknown material field was not rejected at its path");
        }},
        Case{"numeric strings are not coerced", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {fluid = {glass_level = "0.5"}},
                window_rules = {},
                layer_rules = {},
            })");
            const auto parsed = parseLuaConfig(state.get(), -1);
            require(!parsed && parsed.error().path == "materials.fluid.glass_level",
                    "numeric string was silently coerced");
        }},
        Case{"metatables cannot synthesize configuration fields", [] {
            auto state = evaluate(R"(
                local config = {
                    version = 2,
                    enabled = true,
                    materials = {fluid = {}},
                    window_rules = {},
                    layer_rules = {},
                }
                return setmetatable(config, {
                    __index = function(_, key)
                        if key == "default_material" then
                            return "fluid"
                        end
                    end,
                })
            )");
            const auto parsed = parseLuaConfig(state.get(), -1);
            require(!parsed && parsed.error().path == "default_material",
                    "metatable-generated required field was accepted");
        }},
        Case{"rule arrays must be contiguous", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {fluid = {}},
                window_rules = {
                    [2] = {
                        id = "files",
                        match = {initial_class = {exact = "org.gnome.Nautilus"}},
                        material = "fluid",
                    },
                },
                layer_rules = {},
            })");
            const auto parsed = parseLuaConfig(state.get(), -1);
            require(!parsed && parsed.error().path == "window_rules",
                    "sparse array was silently accepted");
        }},
        Case{"match expressions require one mode", [] {
            auto state = evaluate(R"(return {
                version = 2,
                enabled = true,
                default_material = "fluid",
                materials = {fluid = {}},
                window_rules = {{
                    id = "files",
                    match = {
                        initial_class = {
                            exact = "org.gnome.Nautilus",
                            regex = "^org%.gnome",
                        },
                    },
                    material = "fluid",
                }},
                layer_rules = {},
            })");
            const auto parsed = parseLuaConfig(state.get(), -1);
            require(!parsed && parsed.error().path == "window_rules[0].match.initial_class",
                    "ambiguous match expression was accepted");
        }},
    });
}
