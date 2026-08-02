// ─────────────────────────────────────────────────────────────────────────
//  HyprFluidGlass — live fluid-glass compositor material for Hyprland
//
//  The v2 runtime owns everything: a client (HyprGlassShell) opens a session
//  over the `hyprfluidglass` hyprctl command and publishes targets that name
//  a compositor surface by selector. The plugin derives each target's
//  geometry from the live surface every frame, captures the framebuffer
//  behind it, and runs the fluid-glass shader over the capture. Lua config
//  rules provide durable glass for surfaces no client manages.
//
//  There is no push channel: clients poll `status` for the per-output
//  liveness rows that gate their own presentation. The single `hgsglass`
//  event left is the farewell on unload, so a client can drop to its neutral
//  material immediately instead of waiting out a poll interval.
// ─────────────────────────────────────────────────────────────────────────

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <nlohmann/json.hpp>
#include <lua.hpp>

#include "v2/config/LuaConfig.hpp"
#include "v2/core/OpaqueId.hpp"
#include "v2/runtime/HyprlandGlassSceneController.hpp"
#include "v2/runtime/Runtime.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#ifndef HYPRFLUIDGLASS_PLUGIN_VERSION
#define HYPRFLUIDGLASS_PLUGIN_VERSION "0.1.0"
#endif

namespace {

using json = nlohmann::json;

HANDLE                                                 g_handle = nullptr;
SP<SHyprCtlCommand>                                    g_v2Command;
CHyprSignalListener                                    g_renderStageListener;
CHyprSignalListener                                    g_v2PreChecksListener;
CHyprSignalListener                                    g_v2ConfigPreReloadListener;
CHyprSignalListener                                    g_v2ConfigReloadedListener;
std::unique_ptr<hfg::v2::RuntimeService>               g_v2Runtime;
std::shared_ptr<hfg::v2::HyprlandGlassSceneController> g_v2Controller;
SP<CEventLoopTimer>                                    g_refreshTimer;
bool                                                   g_v2LuaFunctionRegistered = false;
// Unique-per-load generation nonce (steady-clock ms, within JS's safe-integer
// range so the shell compares it exactly). The farewell carries it so a client
// can tell an orderly unload from a stale event.
uint64_t                                               g_pluginGen = 0;

double nowSteadyMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint64_t nowMonotonicMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void recordBoundaryFailure(std::string_view boundary, const char* detail) noexcept {
    try {
        Log::logger->log(Hyprutils::CLI::LOG_ERR,
                         "[hyprfluidglass] {} failed: {}", boundary,
                         detail ? detail : "unknown exception");
    } catch (...) {
    }
}

std::string trim(std::string v) {
    const auto a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const auto b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

std::string removePrefix(std::string v, const std::string& p) {
    if (v.rfind(p, 0) == 0) v = v.substr(p.size());
    return trim(std::move(v));
}

void damageAllMonitors() {
    if (!g_pHyprRenderer) return;
    for (const auto& m : State::monitorState()->monitors())
        if (m) g_pHyprRenderer->damageMonitor(m);
}

// Whether any work could possibly need a render tick: live session targets,
// enabled config rules, or renderer resources that still have to wind down.
bool v2HasPotentialScene() {
    if (!g_v2Runtime)
        return false;
    if (g_v2Runtime->sessionManager().targetCount() != 0U)
        return true;
    const auto* config = g_v2Runtime->configStore().active();
    if (config && config->enabled &&
        (!config->windowRules.empty() || !config->layerRules.empty()))
        return true;
    const auto& renderer = g_v2Runtime->rendererStatus();
    return !renderer.renderingReady ||
        renderer.presentations != 0U ||
        renderer.captureResources != 0U ||
        renderer.draws != 0U ||
        renderer.windowAttachments != 0U ||
        renderer.directScanoutLeases != 0U;
}

void renderFluidGlass(eRenderStage stage) {
    try {
        if (!g_v2Controller || !v2HasPotentialScene())
            return;
        if (auto rendered = g_v2Controller->onRenderStage(stage); !rendered)
            recordBoundaryFailure("v2-render-stage",
                                  rendered.error().message.c_str());
    } catch (const std::exception& error) {
        recordBoundaryFailure("v2-render-stage", error.what());
    } catch (...) {
        recordBoundaryFailure("v2-render-stage", "non-standard exception");
    }
}

std::string onV2(eHyprCtlOutputFormat, std::string req) {
    static constexpr std::string_view UNAVAILABLE =
        R"({"ok":false,"version":2,"error":{"code":"internal-error","path":"","message":"runtime is unavailable"}})";
    try {
        if (!g_v2Runtime)
            return std::string(UNAVAILABLE);
        const auto nowMs = nowMonotonicMs();
        auto payload = removePrefix(std::move(req), "hyprfluidglass");
        const auto request = json::parse(payload, nullptr, false, true);
        const auto operation =
            request.is_object() && request.contains("operation") &&
                    request["operation"].is_string()
                ? request["operation"].get<std::string>()
                : std::string{};
        const bool changesScene =
            operation == "session.replace" ||
            operation == "session.close";
        auto response = g_v2Runtime->handle(payload, nowMs);
        if (changesScene && g_v2Controller) {
            const auto parsed = json::parse(
                response, nullptr, false, true);
            if (!parsed.is_discarded() &&
                parsed.value("ok", false)) {
                const auto refreshed =
                    g_v2Controller->refresh(nowMs);
                if (!refreshed)
                    recordBoundaryFailure(
                        "v2-request-refresh",
                        refreshed.error().message.c_str());
                damageAllMonitors();
            }
        }
        return response;
    } catch (...) {
        return std::string(UNAVAILABLE);
    }
}

int onV2LuaConfigure(lua_State* state) {
    bool failed = false;
    {
        std::string message;
        try {
            if (!g_v2Runtime) {
                message = "hyprfluidglass runtime is unavailable";
            } else if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TTABLE) {
                message = "configure expects exactly one configuration table";
            } else {
                auto parsed = hfg::v2::parseLuaConfig(state, 1);
                if (!parsed) {
                    const auto& error = parsed.error();
                    message = error.path.empty() ? error.message : error.path + ": " + error.message;
                } else {
                    auto staged = g_v2Runtime->configStore().stage(std::move(parsed.value()));
                    if (!staged) {
                        const auto& error = staged.error();
                        message = error.path.empty() ? error.message : error.path + ": " + error.message;
                    }
                }
            }
        } catch (...) {
            message = "internal hyprfluidglass configuration failure";
        }

        if (!message.empty()) {
            lua_pushlstring(state, message.data(), message.size());
            failed = true;
        }
    }

    if (failed)
        return lua_error(state);
    lua_pushboolean(state, 1);
    return 1;
}

void onV2ConfigPreReload() noexcept {
    try {
        if (g_v2Runtime)
            g_v2Runtime->configStore().beginReload();
    } catch (...) {
    }
}

void onV2ConfigReloaded() noexcept {
    try {
        if (g_v2Runtime) {
            const auto committed = g_v2Runtime->configStore().commitReload();
            if (committed && g_v2Controller) {
                const auto refreshed =
                    g_v2Controller->refresh(nowMonotonicMs());
                if (!refreshed)
                    recordBoundaryFailure(
                        "v2-config-refresh",
                        refreshed.error().message.c_str());
                damageAllMonitors();
            }
        }
    } catch (...) {
    }
}

} // namespace

// ── Plugin lifecycle ──────────────────────────────────────────────────────
APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;
    g_v2Runtime = std::make_unique<hfg::v2::RuntimeService>(hfg::v2::secureOpaqueId);
    {
        const auto controller =
            hfg::v2::HyprlandGlassSceneController::create(
                handle, *g_v2Runtime,
                {
                    .maxApronPixels = 2048U,
                    .maxPixels = 64U * 1024U * 1024U,
                    .maxBytes = 256U * 1024U * 1024U,
                    .maxTotalBytes = 512U * 1024U * 1024U,
                });
        if (controller)
            g_v2Controller = controller.value();
        else {
            g_v2Runtime->setRendererStatus({
                .renderingReady = false,
                .renderer = "failed",
                .lastError = controller.error(),
            });
            recordBoundaryFailure("v2-controller-init",
                                  controller.error().message.c_str());
        }
    }

    g_pluginGen = nowMonotonicMs();

    SHyprCtlCommand v2; v2.name = "hyprfluidglass"; v2.exact = false; v2.fn = onV2;
    g_v2Command = HyprlandAPI::registerHyprCtlCommand(g_handle, v2);

    if (Event::bus()) {
        g_v2LuaFunctionRegistered =
            HyprlandAPI::addLuaFunction(g_handle, "hyprfluidglass", "configure", onV2LuaConfigure);
        if (g_v2LuaFunctionRegistered) {
            g_v2ConfigPreReloadListener =
                Event::bus()->m_events.config.preReload.listen(onV2ConfigPreReload);
            g_v2ConfigReloadedListener =
                Event::bus()->m_events.config.reloaded.listen(onV2ConfigReloaded);
            HyprlandAPI::reloadConfig();
        }
    }

    if (g_pEventLoopManager) {
        // Lease expiry and scene refresh both ride this timer. Brisk while a
        // scene could exist; slow when idle so an empty compositor pays ~1Hz.
        g_refreshTimer = makeShared<CEventLoopTimer>(std::chrono::milliseconds(200), [](SP<CEventLoopTimer> self, void*) {
            try {
                if (g_v2Controller && v2HasPotentialScene()) {
                    const auto refreshed =
                        g_v2Controller->refresh(nowMonotonicMs());
                    if (!refreshed)
                        recordBoundaryFailure(
                            "v2-runtime-tick",
                            refreshed.error().message.c_str());
                } else if (g_v2Runtime) {
                    g_v2Runtime->tick(nowMonotonicMs());
                }
                if (self)
                    self->updateTimeout(
                        v2HasPotentialScene()
                            ? std::chrono::milliseconds(200)
                            : std::chrono::milliseconds(1000));
            } catch (const std::exception& error) {
                recordBoundaryFailure("v2-runtime-tick", error.what());
                if (self)
                    self->updateTimeout(std::chrono::seconds(1));
            } catch (...) {
                recordBoundaryFailure("v2-runtime-tick", "non-standard exception");
                if (self)
                    self->updateTimeout(std::chrono::seconds(1));
            }
        }, nullptr);
        g_pEventLoopManager->addTimer(g_refreshTimer);
    }

    if (Event::bus()) {
        if (g_v2Controller)
            g_v2PreChecksListener =
                Event::bus()->m_events.render.preChecks.listen(
                    [](PHLMONITOR monitor) {
                        try {
                            if (!g_v2Controller ||
                                !v2HasPotentialScene())
                                return;
                            if (auto checked =
                                    g_v2Controller->onPreChecks(
                                        std::move(monitor), nowMonotonicMs());
                                !checked)
                                recordBoundaryFailure(
                                    "v2-render-precheck",
                                    checked.error().message.c_str());
                        } catch (const std::exception& error) {
                            recordBoundaryFailure("v2-render-precheck",
                                                  error.what());
                        } catch (...) {
                            recordBoundaryFailure(
                                "v2-render-precheck",
                                "non-standard exception");
                        }
                    });
        g_renderStageListener = Event::bus()->m_events.render.stage.listen(renderFluidGlass);
    }

    return {"hyprfluidglass", "Live fluid-glass compositor material for Hyprland", "CoastLineSec", HYPRFLUIDGLASS_PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_v2ConfigReloadedListener.reset();
    g_v2ConfigPreReloadListener.reset();
    if (g_v2LuaFunctionRegistered)
        HyprlandAPI::removeLuaFunction(g_handle, "hyprfluidglass", "configure");
    g_v2LuaFunctionRegistered = false;
    g_renderStageListener.reset();
    g_v2PreChecksListener.reset();
    if (g_v2Controller) {
        g_v2Controller->clear();
        g_v2Controller.reset();
    }
    // Farewell event so the shell learns of an orderly teardown immediately:
    // its glass gate must drop without waiting out a poll interval.
    if (g_pEventManager) {
        json bye = {{"v", 1}, {"gen", g_pluginGen}, {"pluginLoaded", false}, {"kind", "farewell"},
                    {"enabled", false}, {"tMs", nowSteadyMs()}, {"activeDescriptors", 0}, {"descriptors", json::array()}};
        g_pEventManager->postEvent(SHyprIPCEvent{"hgsglass", bye.dump(-1, ' ', false, json::error_handler_t::replace)});
    }
    if (g_refreshTimer) {
        g_refreshTimer->cancel();
        if (g_pEventLoopManager)
            g_pEventLoopManager->removeTimer(g_refreshTimer);
        g_refreshTimer.reset();
    }
    if (g_v2Command)
        HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_v2Command);
    g_v2Command.reset();
    g_v2Runtime.reset();
    g_handle = nullptr;
}
