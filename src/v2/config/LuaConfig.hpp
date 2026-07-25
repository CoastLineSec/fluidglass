#pragma once

#include "v2/model/Config.hpp"

struct lua_State;

namespace hfg::v2 {

[[nodiscard]] Result<ConfigSnapshotInput> parseLuaConfig(lua_State* state, int index);

} // namespace hfg::v2
