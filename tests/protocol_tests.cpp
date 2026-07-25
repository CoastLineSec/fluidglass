#include "TestHarness.hpp"

#include "v2/ipc/Protocol.hpp"

#include <nlohmann/json.hpp>

#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"capabilities request", [] {
            const auto result = parseRequest(R"({"version":2,"operation":"capabilities","request_id":"one"})");
            require(result.hasValue(), "capabilities request failed");
            require(std::holds_alternative<CapabilitiesRequest>(result.value().body), "wrong request type");
            require(result.value().requestId == "one", "request id changed");
        }},
        Case{"invalid envelopes", [] {
            require(!parseRequest("{"), "invalid JSON must fail");
            require(!parseRequest("[]"), "array request must fail");
            require(!parseRequest(R"({"version":1,"operation":"status"})"), "wrong version must fail");
            require(!parseRequest(R"({"version":2,"operation":"unknown"})"), "unknown operation must fail");
            const auto extra = parseRequest(R"({"version":2,"operation":"status","extra":true})");
            require(!extra && extra.error().path == "extra", "unknown field must fail at its path");
        }},
        Case{"duplicate fields and nesting limits", [] {
            const auto duplicate = parseRequest(
                R"({"version":2,"operation":"status","operation":"capabilities"})");
            require(!duplicate && duplicate.error().message == "duplicate field",
                    "duplicate request fields must fail");

            std::string nested = R"({"version":2,"operation":"status","extra":)";
            nested.append(66, '[');
            nested += "null";
            nested.append(66, ']');
            nested += '}';
            const auto tooDeep = parseRequest(nested);
            require(!tooDeep && tooDeep.error().code == ErrorCode::ResourceLimited,
                    "excessive JSON nesting must be resource-limited");
        }},
        Case{"session lifecycle requests", [] {
            const auto open = parseRequest(R"({"version":2,"operation":"session.open","client_id":"shell","mode":"preview"})");
            require(open.hasValue(), "open request failed");
            require(std::get<OpenSessionRequest>(open.value().body).mode == SessionMode::Preview, "preview mode changed");

            const auto heartbeat = parseRequest(R"({
                "version":2,"operation":"session.heartbeat",
                "session_id":"one","token":"two","generation":3
            })");
            require(heartbeat.hasValue(), "heartbeat request failed");
            require(std::get<HeartbeatSessionRequest>(heartbeat.value().body).generation == 3, "generation changed");

            require(parseRequest(R"({
                "version":2,"operation":"session.close","session_id":"one","token":"two"
            })").hasValue(), "close request failed");
            require(parseRequest(R"({
                "version":2,"operation":"target.inspect",
                "session_id":"one","token":"two","target_id":"bar"
            })").hasValue(), "inspect request failed");
        }},
        Case{"region replacement", [] {
            const auto result = parseRequest(R"({
                "version":2,
                "operation":"session.replace",
                "session_id":"one",
                "token":"two",
                "generation":1,
                "materials":{
                    "fluid":{
                        "glass_level":0.5,
                        "tint_enabled":true,
                        "tint_color":"#3366CC"
                    }
                },
                "targets":[{
                    "id":"preview",
                    "kind":"region",
                    "selector":{"output":"DP-1"},
                    "geometry":{"space":"output-logical","x":10,"y":20,"width":400,"height":300},
                    "stage":"post-windows",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":20}
                }]
            })");
            require(result.hasValue(), "valid replacement failed");
            const auto& request = std::get<ReplaceSessionRequest>(result.value().body);
            require(request.replacement.generation == 1, "replacement generation changed");
            require(request.replacement.materials.size() == 1, "material was not parsed");
            require(request.replacement.targets.size() == 1, "target was not parsed");
            require(request.replacement.targets[0].kind == TargetKind::Region, "region kind changed");
        }},
        Case{"window and layer targets", [] {
            const auto result = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[
                    {
                        "id":"files","kind":"window",
                        "selector":{"address":"0x1234","pid":42,"initial_class":"org.gnome.Nautilus"},
                        "material":{"source":"session","name":"fluid"},
                        "shape":{"kind":"rounded-rect","radius":12}
                    },
                    {
                        "id":"bar","kind":"layer",
                        "selector":{"namespace":"example-shell:bar"},
                        "geometry":{"space":"surface-local","x":0,"y":0,"width":1000,"height":44},
                        "material":{"source":"session","name":"fluid"},
                        "shape":{"kind":"ring","outer_radius":22,"thickness":3}
                    }
                ]
            })");
            require(result.hasValue(), "window/layer replacement failed");
            const auto& targets = std::get<ReplaceSessionRequest>(result.value().body).replacement.targets;
            require(targets[0].kind == TargetKind::Window, "window kind changed");
            require(targets[1].kind == TargetKind::Layer, "layer kind changed");
        }},
        Case{"compound shape", [] {
            const auto result = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"frame","kind":"region",
                    "selector":{"output":"DP-1"},
                    "geometry":{"space":"output-logical","x":0,"y":0,"width":1000,"height":800},
                    "stage":"post-windows",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"compound","parts":[
                        {"x":0,"y":0,"width":1000,"height":44,"radius":22},
                        {"x":0,"y":756,"width":1000,"height":44,"radius":22}
                    ]}
                }]
            })");
            require(result.hasValue(), "compound target failed");
            const auto& target = std::get<ReplaceSessionRequest>(result.value().body).replacement.targets[0];
            require(std::get<CompoundShape>(target.shape).parts.size() == 2, "compound parts changed");
        }},
        Case{"strict material fields", [] {
            const auto wrongType = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{"glass_level":"high"}},"targets":[]
            })");
            require(!wrongType && wrongType.error().path == "materials.fluid.glass_level",
                    "wrong material type was not rejected precisely");
            require(wrongType.error().code == ErrorCode::InvalidMaterial,
                    "material failures need the material error code");
            require(!parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{"unknown":1}},"targets":[]
            })"), "unknown material field must fail");
        }},
        Case{"strict target fields and spaces", [] {
            const auto badSpace = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"preview","kind":"region",
                    "selector":{"output":"DP-1"},
                    "geometry":{"space":"surface-local","x":0,"y":0,"width":10,"height":10},
                    "stage":"post-windows",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })");
            require(!badSpace && badSpace.error().path == "targets[0].geometry.space",
                    "wrong geometry space was not rejected precisely");
            require(!parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"files","kind":"window",
                    "selector":{"address":"0x1234"},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })"), "unguarded window target must fail");
        }},
        Case{"target errors preserve target paths", [] {
            const auto invalidId = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"bad id","kind":"layer",
                    "selector":{"namespace":"example-shell:bar"},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })");
            require(!invalidId && invalidId.error().code == ErrorCode::InvalidTarget,
                    "target validation needs the target error code");
            require(invalidId.error().path == "targets[0].id",
                    "target validation path lost its array location");
        }},
        Case{"forbidden per-kind fields are not ignored", [] {
            const auto windowGeometry = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"files","kind":"window",
                    "selector":{"address":"0x1234","pid":42},
                    "geometry":{"space":"surface-local","x":0,"y":0,"width":10,"height":10},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })");
            require(!windowGeometry && windowGeometry.error().path == "targets[0].geometry",
                    "window geometry must not be silently ignored");

            const auto layerStage = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"bar","kind":"layer",
                    "selector":{"namespace":"example-shell:bar"},
                    "stage":"post-layer",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })");
            require(!layerStage && layerStage.error().path == "targets[0].stage",
                    "layer stage must not be silently ignored");
        }},
        Case{"window pid range", [] {
            const auto tooLarge = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"files","kind":"window",
                    "selector":{"address":"0x1234","pid":18446744073709551615},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }]
            })");
            require(!tooLarge && tooLarge.error().path == "targets[0].selector.pid",
                    "unsigned pid overflow must fail at its field");
        }},
        Case{"request size limit", [] {
            std::string oversized(262'145, ' ');
            const auto result = parseRequest(oversized);
            require(!result && result.error().code == ErrorCode::ResourceLimited, "oversized request must be resource-limited");
        }},
        Case{"response envelopes", [] {
            const auto success = nlohmann::json::parse(successResponse("one", {{"value", 42}}));
            require(success["ok"] == true && success["request_id"] == "one", "success envelope changed");
            const auto failed = nlohmann::json::parse(failureResponse("two", {
                .code = ErrorCode::InvalidTarget,
                .path = "targets[0]",
                .message = "bad target",
            }));
            require(failed["ok"] == false, "failure envelope changed");
            require(failed["error"]["code"] == "invalid-target", "error code changed");
            require(failed["request_id"] == "two", "failure request id changed");
        }},
    });
}
