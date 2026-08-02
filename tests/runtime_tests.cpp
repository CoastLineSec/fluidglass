#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/runtime/Runtime.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

using json = nlohmann::json;

class IdSequence {
  public:
    std::string next() {
        return "opaque-" + std::to_string(++m_value);
    }

  private:
    std::uint64_t m_value = 0;
};

struct Fixture {
    IdSequence ids;
    RuntimeService runtime{[this] {
        return ids.next();
    }};
};

json call(RuntimeService& runtime, std::string_view request, std::uint64_t nowMs = 0) {
    return json::parse(runtime.handle(request, nowMs));
}

json open(RuntimeService& runtime, std::string_view client = "shell", std::uint64_t nowMs = 0) {
    return call(runtime, std::string(
        R"({"version":2,"operation":"session.open","client_id":")") +
        std::string(client) + R"(","mode":"client"})", nowMs);
}

std::string replacement(
    std::string_view sessionId,
    std::string_view token,
    std::uint64_t generation,
    std::string_view materialSource = "session",
    std::string_view materialName = "glass") {
    return std::string(R"({
        "version":2,
        "operation":"session.replace",
        "session_id":")") + std::string(sessionId) +
        R"(","token":")" + std::string(token) +
        R"(","generation":)" + std::to_string(generation) +
        R"(,"materials":{"glass":{}},"targets":[{
            "id":"bar",
            "kind":"region",
            "selector":{"output":"DP-1"},
            "geometry":{"space":"output-logical","x":0,"y":0,"width":800,"height":44},
            "stage":"post-windows",
            "material":{"source":")" + std::string(materialSource) +
        R"(","name":")" + std::string(materialName) +
        R"("},"shape":{"kind":"rounded-rect","radius":22}
        }]})";
}

std::string compoundReplacement(std::string_view sessionId, std::string_view token) {
    return std::string(R"({
        "version":2,
        "operation":"session.replace",
        "session_id":")") + std::string(sessionId) +
        R"(","token":")" + std::string(token) +
        R"(","generation":1,
        "materials":{"glass":{}},
        "targets":[{
            "id":"frame",
            "kind":"region",
            "selector":{"output":"DP-1"},
            "geometry":{"space":"output-logical","x":0,"y":0,"width":800,"height":600},
            "stage":"post-windows",
            "material":{"source":"session","name":"glass"},
            "shape":{
                "kind":"compound",
                "base":{"radius":24},
                "cutout":{
                    "x":40,"y":40,"width":720,"height":520,
                    "corner_radii":{
                        "top_left":18,"top_right":18,
                        "bottom_right":14,"bottom_left":14
                    }
                },
                "parts":[{
                    "x":0,"y":0,"width":800,"height":44,"radius":22,
                    "junctions":{
                        "top_left":0,"top_right":7,
                        "bottom_right":7,"bottom_left":0
                    },
                    "material_extent":{"x":-8,"y":-8,"width":816,"height":60},
                    "transition":{
                        "id":"frame-part-enter-1",
                        "phase":"enter",
                        "edge":"top",
                        "duration_ms":180,
                        "elapsed_ms":20,
                        "travel":44,
                        "protrusion":52,
                        "easing":[]
                    },
                    "opacity":0.75
                }],
                "connectors":[{"x":16,"y":40,"width":12,"height":12}],
                "connector_curve":6
            },
            "transition":{
                "id":"frame-enter-1",
                "phase":"enter",
                "edge":"bottom",
                "duration_ms":240,
                "elapsed_ms":40,
                "travel":44,
                "easing":[{
                    "control1_x":0.2,"control1_y":0,
                    "control2_x":0.3,"control2_y":1,
                    "end_x":1,"end_y":1
                }]
            }
        }]
    })";
}

std::string layerReplacement(
    std::string_view sessionId,
    std::string_view token,
    std::uint64_t generation,
    bool handoff = false,
    std::uint64_t sourceGeneration = 0) {
    return std::string(R"({
        "version":2,
        "operation":"session.replace",
        "session_id":")") + std::string(sessionId) +
        R"(","token":")" + std::string(token) +
        R"(","generation":)" + std::to_string(generation) +
        R"(,"materials":{"glass":{}},"targets":[{
            "id":"bar","kind":"layer",
            "selector":{"namespace":"hgs:bar:DP-1"},
            "geometry":{"space":"surface-local","x":0,"y":0,"width":800,"height":44},
            "material":{"source":"session","name":"glass"},
            "shape":{"kind":"rounded-rect","radius":18}
        }])" +
        (handoff
             ? std::string(R"(,"handoffs":[{
                    "target_id":"bar","source_generation":)") +
                   std::to_string(sourceGeneration) +
                   R"(,"mode":"retain-until-drawn","timeout_ms":500}])"
             : std::string{}) +
        "}";
}

std::string dockReplacement(
    std::string_view sessionId,
    std::string_view token,
    std::uint64_t generation) {
    return std::string(R"({
        "version":2,
        "operation":"session.replace",
        "session_id":")") + std::string(sessionId) +
        R"(","token":")" + std::string(token) +
        R"(","generation":)" + std::to_string(generation) +
        R"(,"materials":{"glass":{}},"targets":[{
            "id":"dock","kind":"layer",
            "selector":{"namespace":"hgs:dock:DP-1"},
            "material":{"source":"session","name":"glass"},
            "shape":{"kind":"rounded-rect","radius":18}
        }]})";
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"capabilities report the inert runtime", [] {
            Fixture fixture;
            const auto result = call(fixture.runtime, R"({"version":2,"operation":"capabilities"})");
            require(result["ok"] == true, "capabilities failed");
            require(result["result"]["rendering_ready"] == false, "inert renderer was advertised as ready");
            require(result["result"]["limits"]["request_bytes"] == 262144, "request limit changed");
            require(result["result"]["limits"]["compound_connectors"] == 32, "connector limit is missing");
            require(result["result"]["limits"]["bezier_segments"] == 16, "Bezier limit is missing");
            require(result["result"]["limits"]["transition_ms"] == 60000, "transition limit is missing");
            require(result["result"]["transitions"]["compound_parts"] == true,
                    "part transitions are not advertised");
            // The retired handoff/morph/visibility capability blocks must
            // stay gone: their presence is what let two protocols coexist.
            require(!result["result"].contains("presentation_handoffs") &&
                        !result["result"].contains("visibility_transitions") &&
                        !result["result"]["limits"]
                              .contains("presentation_handoff_ms") &&
                        !result["result"]["limits"]
                              .contains("presentation_morph_ms") &&
                        !result["result"]["limits"]
                              .contains("visibility_transition_ms"),
                    "a retired capability block is still advertised");
            require(
                result["result"]["target_readiness"]["inactive_reporting"] ==
                        true &&
                    result["result"]["target_readiness"]["detail"] == true &&
                    result["result"]["target_readiness"]["inactive_reasons"] ==
                        json::array({
                            "disabled",
                            "empty-geometry",
                            "offscreen",
                            "suppressed",
                        }),
                "inactive target reporting is not advertised");
        }},
        Case{"an inactive target is reported instead of waiting forever", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime, replacement(sessionId, token, 1))["ok"] == true,
                    "session replacement failed");
            const auto owner =
                fixture.runtime.sessionManager().snapshots().front().owner;
            require(fixture.runtime.readinessTracker()
                        .failTarget(
                            {.owner = owner, .targetId = "bar"},
                            ReadinessState::Inactive,
                            std::string(targetInactiveReasonDetail(
                                TargetInactiveReason::Offscreen)))
                        .hasValue(),
                    "a resolved target could not be reported inactive");

            const auto inspect = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"target.inspect","session_id":")") +
                sessionId + R"(","token":")" + token + R"(","target_id":"bar"})");
            require(inspect["result"]["state"] == "inactive",
                    "an inactive target still inspects as accepted");
            require(inspect["result"]["presentations"].empty(),
                    "an inactive target reported a presentation");
            require(inspect["result"]["detail"] ==
                        "target intersects no current output",
                    "an inactive target did not report why it is inactive");

            const auto status = call(
                fixture.runtime, R"({"version":2,"operation":"status"})");
            require(status["result"]["readiness"]["inactive"] == 1,
                    "status did not aggregate the inactive target");
        }},
        Case{"capabilities and status report the live renderer truthfully", [] {
            Fixture fixture;
            fixture.runtime.setRendererStatus({
                .renderingReady = true,
                .renderer = "active",
                .presentations = 3,
                .captureResources = 2,
                .draws = 3,
                .windowAttachments = 1,
                .directScanoutLeases = 2,
                .lastError = std::nullopt,
            });
            const auto capabilities = call(
                fixture.runtime,
                R"({"version":2,"operation":"capabilities"})");
            require(capabilities["result"]["rendering_ready"] == true,
                    "live renderer was advertised as inert");
            const auto status = call(
                fixture.runtime,
                R"({"version":2,"operation":"status"})");
            require(status["result"]["renderer"]["state"] == "active",
                    "renderer state was not published");
            require(status["result"]["renderer"]["presentations"] == 3,
                    "presentation count was not published");
            require(status["result"]["renderer"]["capture_resources"] == 2,
                    "capture count was not published");
            require(status["result"]["renderer"]["last_error"].is_null(),
                    "successful renderer published an error");
        }},
        Case{"renderer failures are structured and sanitized", [] {
            Fixture fixture;
            fixture.runtime.setRendererStatus({
                .renderingReady = false,
                .renderer = "failed",
                .lastError = Error{
                    .code = ErrorCode::UnsupportedOperation,
                    .path = "renderer",
                    .message = "OpenGL renderer is unavailable",
                },
            });
            const auto status = call(
                fixture.runtime,
                R"({"version":2,"operation":"status"})");
            const auto& renderer = status["result"]["renderer"];
            require(renderer["state"] == "failed",
                    "renderer failure state was lost");
            require(renderer["last_error"]["code"] ==
                        "unsupported-operation",
                    "renderer error code was lost");
            require(renderer["last_error"]["path"] == "renderer",
                    "renderer error path was lost");
        }},
        Case{"session lifecycle and privacy-safe status", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            require(opened["ok"] == true, "session open failed");
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();

            const auto replaced = call(fixture.runtime, replacement(sessionId, token, 1), 10);
            require(replaced["ok"] == true, "session replacement failed");
            require(replaced["result"]["targets"] == 1, "replacement target count changed");

            const auto status = call(fixture.runtime, R"({"version":2,"operation":"status"})", 11);
            require(status["result"]["totals"]["sessions"] == 1, "status lost the session");
            require(status["result"]["readiness"]["accepted"] == 1, "accepted target readiness is missing");
            require(!status["result"].contains("presentation_handoffs") &&
                        !status["result"].contains("visibility_transitions"),
                    "status still reports a retired protocol aggregate");
            require(status.dump().find(token) == std::string::npos, "status leaked a session token");

            const auto heartbeat = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"session.heartbeat","session_id":")") +
                sessionId + R"(","token":")" + token + R"(","generation":1})", 20);
            require(heartbeat["ok"] == true, "heartbeat failed");
            require(heartbeat.dump().find(token) == std::string::npos,
                    "heartbeat response repeated the owner token");

            const auto closed = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"session.close","session_id":")") +
                sessionId + R"(","token":")" + token + R"("})", 21);
            require(closed["ok"] == true && closed["result"]["closed"] == true, "close failed");
            const auto after = call(fixture.runtime, R"({"version":2,"operation":"status"})", 22);
            require(after["result"]["totals"]["sessions"] == 0, "closed session remained live");
            require(after["result"]["readiness"].empty(), "closed target readiness remained live");
        }},
        Case{"target inspection requires the owner token", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime, replacement(sessionId, token, 1))["ok"] == true,
                    "session replacement failed");

            const auto inspect = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"target.inspect","session_id":")") +
                sessionId + R"(","token":")" + token + R"(","target_id":"bar"})");
            require(inspect["ok"] == true, "target inspection failed");
            require(inspect["result"]["target"]["selector"]["output"] == "DP-1",
                    "target definition was not returned");
            require(inspect["result"]["state"] == "accepted", "target readiness changed");

            const auto denied = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"target.inspect","session_id":")") +
                sessionId + R"(","token":"wrong","target_id":"bar"})");
            require(denied["ok"] == false, "wrong inspection token was accepted");
            require(denied["error"]["code"] == "invalid-token", "wrong inspection error code");
        }},
        Case{"capabilities advertise continuous output liveness", [] {
            Fixture fixture;
            const auto result = call(
                fixture.runtime,
                R"({"version":2,"operation":"capabilities"})", 1);
            require(result["ok"] == true, "capabilities request failed");
            const auto& liveness =
                result["result"]["output_liveness"];
            require(liveness["rows"] == true &&
                        liveness["continuous"] == true,
                    "output liveness is not advertised");
        }},
        Case{"status serves a liveness row for every renderer-known output", [] {
            Fixture fixture;
            fixture.runtime.setRendererStatus({
                .renderingReady = true,
                .renderer = "active",
                .outputs = {{.name = "DP-1", .generation = 3}},
            });
            const auto status = call(
                fixture.runtime,
                R"({"version":2,"operation":"status"})", 5);
            require(status["ok"] == true, "status request failed");
            const auto& outputs = status["result"]["outputs"];
            require(outputs.size() == 1U &&
                        outputs[0]["name"] == "DP-1" &&
                        outputs[0]["generation"] == 3 &&
                        outputs[0]["drawing"] == false &&
                        outputs[0]["drawn"] == 0 &&
                        outputs[0]["awaiting"] == 0,
                    "known output did not keep its liveness row");
        }},
        Case{"session.replace keeps readiness for targets the successor retains", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId =
                opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime,
                         layerReplacement(sessionId, token, 1), 10)["ok"] ==
                        true,
                    "initial layer replacement failed");
            const auto snapshot = fixture.runtime.sessionManager().snapshot(
                sessionId);
            require(snapshot.has_value(), "session snapshot is unavailable");
            const TargetIdentity identity{
                .owner = snapshot->owner,
                .targetId = "bar",
            };
            const PresentationKey key{
                .identity = identity,
                .output = "DP-1",
                .outputGeneration = 9,
                .stage = RenderStage::PostLayer,
            };
            auto& readiness = fixture.runtime.readinessTracker();
            require(readiness.resolvePresentation(key).hasValue() &&
                        readiness.transition(key, ReadinessState::Attached)
                            .hasValue() &&
                        readiness.transition(key,
                                             ReadinessState::CaptureReady)
                            .hasValue() &&
                        readiness.transition(key, ReadinessState::Drawn)
                            .hasValue(),
                    "target did not reach drawn");

            // A publish that keeps the target must not blink its liveness:
            // the drawn presentation survives the generation swap untouched.
            require(call(fixture.runtime,
                         layerReplacement(sessionId, token, 2), 20)["ok"] ==
                        true,
                    "retaining replacement failed");
            const auto retained = readiness.presentation(key);
            require(retained.has_value() &&
                        retained->state == ReadinessState::Drawn,
                    "drawn presentation did not survive the replace");
            const auto rows = outputGlassLiveness(readiness);
            require(rows.size() == 1U && rows[0].output == "DP-1" &&
                        rows[0].drawing,
                    "liveness row blinked across the generation swap");
        }},
        Case{"session.replace erases readiness only for dropped targets", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId =
                opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime,
                         layerReplacement(sessionId, token, 1), 10)["ok"] ==
                        true,
                    "initial layer replacement failed");
            const auto snapshot = fixture.runtime.sessionManager().snapshot(
                sessionId);
            require(snapshot.has_value(), "session snapshot is unavailable");
            const TargetIdentity bar{
                .owner = snapshot->owner,
                .targetId = "bar",
            };
            const PresentationKey key{
                .identity = bar,
                .output = "DP-1",
                .outputGeneration = 9,
                .stage = RenderStage::PostLayer,
            };
            auto& readiness = fixture.runtime.readinessTracker();
            require(readiness.resolvePresentation(key).hasValue(),
                    "presentation did not resolve");

            require(call(fixture.runtime,
                         dockReplacement(sessionId, token, 2), 20)["ok"] ==
                        true,
                    "dropping replacement failed");
            require(!readiness.target(bar).has_value(),
                    "dropped target kept its readiness record");
            require(readiness.presentations(bar).empty(),
                    "dropped target kept a presentation record");
            const TargetIdentity dock{
                .owner = snapshot->owner,
                .targetId = "dock",
            };
            const auto accepted = readiness.target(dock);
            require(accepted.has_value() &&
                        accepted->state == ReadinessState::Accepted,
                    "replacement target was not accepted fresh");
        }},
        Case{"retired protocol keys reject the whole replacement", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId =
                opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime,
                         layerReplacement(sessionId, token, 1), 10)["ok"] ==
                        true,
                    "initial layer replacement failed");
            const auto rejected = call(
                fixture.runtime,
                layerReplacement(sessionId, token, 2, true, 1), 20);
            require(rejected["ok"] == false &&
                        rejected["error"]["code"] == "invalid-request" &&
                        fixture.runtime.sessionManager()
                                .snapshot(sessionId)
                                ->generation == 1,
                    "a handoffs key must reject the replacement whole");
        }},
        Case{"compound inspection is lossless", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(fixture.runtime, compoundReplacement(sessionId, token))["ok"] == true,
                    "compound replacement failed");

            const auto inspect = call(fixture.runtime, std::string(
                R"({"version":2,"operation":"target.inspect","session_id":")") +
                sessionId + R"(","token":")" + token + R"(","target_id":"frame"})");
            require(inspect["ok"] == true, "compound inspection failed");
            const auto& shape = inspect["result"]["target"]["shape"];
            require(shape["base"]["radius"] == 24, "compound base was not preserved");
            require(shape["cutout"]["corner_radii"]["bottom_left"] == 14,
                    "compound cutout corners were not preserved");
            require(shape["parts"][0]["junctions"]["top_right"] == 7,
                    "compound junctions were not preserved");
            require(shape["parts"][0]["material_extent"]["x"] == -8,
                    "compound material extent was not preserved");
            require(shape["parts"][0]["transition"]["id"] == "frame-part-enter-1",
                    "compound part transition was not preserved");
            require(shape["parts"][0]["transition"]["protrusion"] == 52,
                    "compound part protrusion was not preserved");
            require(shape["parts"][0]["opacity"] == 0.75, "compound opacity was not preserved");
            require(shape["connectors"][0]["width"] == 12, "compound connector was not preserved");
            require(shape["connector_curve"] == 6, "compound connector curve was not preserved");
            require(inspect["result"]["target"]["transition"]["id"] == "frame-enter-1",
                    "target transition was not preserved");
            require(inspect["result"]["target"]["transition"]["easing"][0]["control1_x"] == 0.2,
                    "target transition easing was not preserved");
        }},
        Case{"config material references use the active snapshot", [] {
            Fixture fixture;
            ConfigSnapshotInput config{
                .version = 2,
                .enabled = true,
                .defaultMaterial = "global",
                .materials = {{"global", {}}},
                .windowRules = {},
                .layerRules = {},
            };
            fixture.runtime.configStore().beginReload();
            require(fixture.runtime.configStore().stage(std::move(config)).hasValue(),
                    "config staging failed");
            require(fixture.runtime.configStore().commitReload().hasValue(),
                    "config commit failed");

            const auto opened = open(fixture.runtime);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            require(call(
                fixture.runtime,
                replacement(sessionId, token, 1, "config", "global"))["ok"] == true,
                "active config material was rejected");
        }},
        Case{"status reports a failed config reload without losing active state", [] {
            Fixture fixture;
            ConfigSnapshotInput config{
                .version = 2,
                .enabled = true,
                .defaultMaterial = "global",
                .materials = {{"global", {}}},
                .windowRules = {},
                .layerRules = {},
            };
            fixture.runtime.configStore().beginReload();
            require(fixture.runtime.configStore().stage(std::move(config)).hasValue(),
                    "config staging failed");
            require(fixture.runtime.configStore().commitReload().hasValue(),
                    "config commit failed");

            ConfigSnapshotInput invalid{
                .version = 2,
                .enabled = true,
                .defaultMaterial = "missing",
                .materials = {{"global", {}}},
                .windowRules = {},
                .layerRules = {},
            };
            fixture.runtime.configStore().beginReload();
            require(!fixture.runtime.configStore().stage(std::move(invalid)),
                    "invalid config unexpectedly staged");
            require(!fixture.runtime.configStore().commitReload(),
                    "invalid config reload unexpectedly committed");

            const auto status = call(fixture.runtime, R"({"version":2,"operation":"status"})");
            require(status["result"]["config"]["active"] == true,
                    "failed reload removed the active config");
            require(status["result"]["config"]["generation"] == 1,
                    "failed reload changed the config generation");
            require(status["result"]["config"]["last_reload_error"]["code"] == "invalid-material",
                    "status omitted the config reload error");
            require(status["result"]["config"]["last_reload_error"]["path"] == "default_material",
                    "status changed the config reload error path");
        }},
        Case{"failed replacement preserves live state and readiness", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            const auto replaced = call(fixture.runtime, replacement(sessionId, token, 1));
            require(replaced["ok"] == true, "initial replacement failed");

            const TargetIdentity identity{
                .owner = replaced["result"]["owner"].get<std::string>(),
                .targetId = "bar",
            };
            const PresentationKey presentation{
                .identity = identity,
                .output = "DP-1",
                .outputGeneration = 1,
                .stage = RenderStage::PostWindows,
            };
            require(fixture.runtime.readinessTracker().resolvePresentation(presentation).hasValue(),
                    "presentation resolve failed");
            require(fixture.runtime.readinessTracker().transition(
                presentation,
                ReadinessState::Attached).hasValue(), "attach transition failed");

            const auto failed = call(
                fixture.runtime,
                replacement(sessionId, token, 2, "session", "missing"));
            require(failed["ok"] == false, "invalid replacement succeeded");
            const auto snapshot = fixture.runtime.sessionManager().snapshot(sessionId);
            require(snapshot && snapshot->generation == 1, "failed replacement advanced the generation");
            const auto record = fixture.runtime.readinessTracker().presentation(presentation);
            require(record && record->state == ReadinessState::Attached,
                    "failed replacement changed presentation readiness");
        }},
        Case{"expiry removes sessions and readiness", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime, "shell", 0);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            const auto replaced = call(
                fixture.runtime,
                replacement(sessionId, token, 1),
                0);
            require(replaced["ok"] == true,
                    "session replacement failed");

            const auto status = call(
                fixture.runtime,
                R"({"version":2,"operation":"status"})",
                Limits::CLIENT_LEASE_MS);
            require(status["result"]["totals"]["sessions"] == 0, "expired session remained live");
            require(status["result"]["readiness"].empty(), "expired target readiness remained live");
        }},
        Case{"render-loop tick expires idle sessions without IPC", [] {
            Fixture fixture;
            const auto opened = open(fixture.runtime, "shell", 0);
            const auto sessionId = opened["result"]["session_id"].get<std::string>();
            const auto token = opened["result"]["token"].get<std::string>();
            const auto replaced = call(
                fixture.runtime,
                replacement(sessionId, token, 1),
                0);
            require(replaced["ok"] == true,
                    "session replacement failed");

            fixture.runtime.tick(Limits::CLIENT_LEASE_MS);
            require(fixture.runtime.sessionManager().sessionCount() == 0U,
                    "idle session survived its render-loop tick");
            require(!fixture.runtime.readinessTracker().target({
                        .owner = replaced["result"]["owner"].get<std::string>(),
                        .targetId = "bar",
                    }),
                    "idle session readiness survived expiry");
        }},
        Case{"malformed requests return protocol failures", [] {
            Fixture fixture;
            const auto result = call(fixture.runtime, "{");
            require(result["ok"] == false, "malformed request succeeded");
            require(result["error"]["code"] == "invalid-json", "malformed request error changed");
        }},
        Case{"unexpected exceptions stay inside the runtime boundary", [] {
            RuntimeService runtime{[]() -> std::string {
                throw std::runtime_error("factory failure");
            }};
            const auto result = call(
                runtime,
                R"({"version":2,"operation":"session.open","client_id":"shell","mode":"client"})");
            require(result["ok"] == false, "throwing factory escaped the runtime boundary");
            require(result["error"]["code"] == "internal-error", "internal failure code changed");
            require(result.dump().find("factory failure") == std::string::npos,
                    "internal exception details leaked");
        }},
    });
}
