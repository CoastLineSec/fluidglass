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
                    "shape":{"kind":"rounded-rect","radius":20},
                    "transition":{
                        "id":"preview-enter-1",
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
            })");
            require(result.hasValue(), "valid replacement failed");
            const auto& request = std::get<ReplaceSessionRequest>(result.value().body);
            require(request.replacement.generation == 1, "replacement generation changed");
            require(request.replacement.materials.size() == 1, "material was not parsed");
            require(request.replacement.targets.size() == 1, "target was not parsed");
            require(request.replacement.targets[0].kind == TargetKind::Region, "region kind changed");
            const auto& transition = request.replacement.targets[0].transition;
            require(transition && transition->id == "preview-enter-1", "target transition changed");
            require(transition->edge == TransitionEdge::Bottom, "transition edge changed");
            require(transition->easing.size() == 1, "transition easing changed");
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
        Case{"presentation handoff request is additive and strict", [] {
            const auto result = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"bar","kind":"layer",
                    "selector":{"namespace":"hgs:bar:DP-1"},
                    "geometry":{"space":"surface-local","x":0,"y":0,"width":800,"height":44},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":18}
                }],
                "handoffs":[{
                    "target_id":"bar",
                    "source_generation":1,
                    "mode":"retain-until-drawn",
                    "timeout_ms":500,
                    "morph":{
                        "transition_id":"attach-1",
                        "duration_ms":240,
                        "easing":"ease-out-cubic",
                        "anchor":"compositor-monotonic"
                    }
                }]
            })");
            require(result.hasValue(), "valid handoff request failed");
            const auto& handoffs = std::get<ReplaceSessionRequest>(
                result.value().body).replacement.handoffs;
            require(handoffs.size() == 1U &&
                        handoffs.front().targetId == "bar" &&
                        handoffs.front().sourceGeneration == 1U &&
                        handoffs.front().timeoutMs == 500U &&
                        handoffs.front().morph &&
                        handoffs.front().morph->transitionId == "attach-1" &&
                        handoffs.front().morph->durationMs == 240U,
                    "handoff fields changed during parsing");

            const auto badEasing = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},"targets":[],
                "handoffs":[{"target_id":"bar","source_generation":1,
                    "mode":"retain-until-drawn","timeout_ms":500,
                    "morph":{"transition_id":"bad","duration_ms":200,
                        "easing":"linear","anchor":"compositor-monotonic"}}]
            })");
            require(!badEasing &&
                        badEasing.error().code ==
                            ErrorCode::UnsupportedOperation,
                    "unsupported morph easing was accepted");

            const auto unsupported = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},"targets":[],
                "handoffs":[{"target_id":"bar","source_generation":1,
                    "mode":"cross-fade","timeout_ms":500}]
            })");
            require(!unsupported &&
                        unsupported.error().code ==
                            ErrorCode::UnsupportedOperation,
                    "unsupported handoff mode was accepted");

            const auto unknown = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},"targets":[],
                "handoffs":[{"target_id":"bar","source_generation":1,
                    "mode":"retain-until-drawn","timeout_ms":500,
                    "secret":"value"}]
            })");
            require(!unknown && unknown.error().path == "handoffs[0].secret",
                    "unknown handoff field bypassed strict parsing");
        }},
        Case{"visibility transition request is additive and strict", [] {
            const auto result = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"bar","kind":"layer",
                    "selector":{"namespace":"hgs:bar:DP-1"},
                    "geometry":{"space":"surface-local","x":0,"y":0,"width":800,"height":44},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":18}
                }],
                "visibility_transitions":[{
                    "target_id":"bar","transition_id":"hide-1",
                    "source_generation":1,"direction":"hide",
                    "edge":"top",
                    "source_rect":{"x":12,"y":8,"width":776,"height":44},
                    "source_radius":18,"travel":52,
                    "duration_ms":220,"easing":"ease-out-cubic",
                    "anchor":"compositor-monotonic",
                    "activation":"first-successful-draw",
                    "timeout_ms":750,"output":"DP-1",
                    "namespace":"hgs:bar:DP-1"
                }]
            })");
            require(result.hasValue(), "valid visibility request failed");
            const auto& transitions =
                std::get<ReplaceSessionRequest>(result.value().body)
                    .replacement.visibilityTransitions;
            require(transitions.size() == 1U &&
                        transitions.front().targetId == "bar" &&
                        transitions.front().edge == TransitionEdge::Top &&
                        transitions.front().sourceRadius == 18.0 &&
                        transitions.front().travel == 52.0,
                    "visibility fields changed while parsing");

            const auto unknown = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{},"targets":[],
                "visibility_transitions":[{
                    "target_id":"bar","transition_id":"hide-1",
                    "source_generation":1,"direction":"hide",
                    "edge":"top",
                    "source_rect":{"x":0,"y":0,"width":1,"height":1},
                    "source_radius":0,"travel":1,"duration_ms":1,
                    "easing":"ease-out-cubic",
                    "anchor":"compositor-monotonic",
                    "activation":"first-successful-draw",
                    "timeout_ms":1,"output":"DP-1",
                    "namespace":"n","secret":"value"
                }]
            })");
            require(!unknown &&
                        unknown.error().path ==
                            "visibility_transitions[0].secret",
                    "unknown visibility field bypassed strict parsing");
        }},
        Case{"output-local morph endpoints parse without changing legacy morphs", [] {
            const auto result = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"bar","kind":"layer",
                    "selector":{"namespace":"hgs:bar:DP-1"},
                    "geometry":{"space":"surface-local","x":0,"y":0,"width":800,"height":44},
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":0}
                }],
                "handoffs":[{
                    "target_id":"bar","source_generation":1,
                    "mode":"retain-until-drawn","timeout_ms":500,
                    "morph":{
                        "transition_id":"bottom-attach",
                        "duration_ms":240,
                        "easing":"ease-out-cubic",
                        "anchor":"compositor-monotonic",
                        "coordinate_space":"output-local",
                        "source":{"rect":{"x":12,"y":48,"width":776,"height":44},"radius":18},
                        "destination":{"rect":{"x":0,"y":56,"width":800,"height":44},"radius":0}
                    }
                }]
            })");
            require(result.hasValue(), "output-local morph failed parsing");
            const auto& morph = *std::get<ReplaceSessionRequest>(
                result.value().body).replacement.handoffs.front().morph;
            require(morph.coordinateSpace ==
                        PresentationHandoffRequest::MorphCoordinateSpace::
                            OutputLocal &&
                        morph.source &&
                        morph.source->rect ==
                            Rect{12.0, 48.0, 776.0, 44.0} &&
                        morph.destination &&
                        morph.destination->radius == 0.0,
                    "output-local endpoint intent changed during parsing");

            const auto missing = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},"targets":[],
                "handoffs":[{"target_id":"bar","source_generation":1,
                    "mode":"retain-until-drawn","timeout_ms":500,
                    "morph":{"transition_id":"bad","duration_ms":200,
                        "easing":"ease-out-cubic",
                        "anchor":"compositor-monotonic",
                        "coordinate_space":"output-local"}}]
            })");
            require(!missing &&
                        missing.error().code == ErrorCode::InvalidRequest,
                    "output-local morph without endpoints was accepted");

            const auto leakedEndpoint = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":2,
                "materials":{"fluid":{}},"targets":[],
                "handoffs":[{"target_id":"bar","source_generation":1,
                    "mode":"retain-until-drawn","timeout_ms":500,
                    "morph":{"transition_id":"bad","duration_ms":200,
                        "easing":"ease-out-cubic",
                        "anchor":"compositor-monotonic",
                        "source":{"rect":{"x":0,"y":0,"width":1,"height":1},"radius":0}}}]
            })");
            require(!leakedEndpoint &&
                        leakedEndpoint.error().code ==
                            ErrorCode::InvalidRequest,
                    "surface-local morph accepted explicit endpoint fields");
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
                    "shape":{
                        "kind":"compound",
                        "base":{"radius":28},
                        "cutout":{
                            "x":32,"y":32,"width":936,"height":736,
                            "corner_radii":{
                                "top_left":20,"top_right":20,
                                "bottom_right":16,"bottom_left":16
                            }
                        },
                        "parts":[
                            {
                                "x":0,"y":0,"width":1000,"height":44,"radius":22,
                                "junctions":{
                                    "top_left":0,"top_right":8,
                                    "bottom_right":8,"bottom_left":0
                                },
                                "material_extent":{"x":-8,"y":-8,"width":1016,"height":60},
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
                            },
                            {
                                "x":0,"y":756,"width":1000,"height":44,
                                "corner_radii":{
                                    "top_left":14,"top_right":14,
                                    "bottom_right":22,"bottom_left":22
                                }
                            }
                        ],
                        "connectors":[
                            {"x":16,"y":40,"width":12,"height":12}
                        ],
                        "connector_curve":7
                    }
                }]
            })");
            require(result.hasValue(), "compound target failed");
            const auto& target = std::get<ReplaceSessionRequest>(result.value().body).replacement.targets[0];
            const auto& shape = std::get<CompoundShape>(target.shape);
            require(shape.base && shape.base->corners.topLeft == 28.0, "compound base changed");
            require(shape.cutout && shape.cutout->corners.bottomLeft == 16.0, "compound cutout changed");
            require(shape.parts.size() == 2, "compound parts changed");
            require(shape.parts[0].junctions.topRight == 8.0, "compound junction changed");
            require(shape.parts[0].materialExtent && shape.parts[0].materialExtent->x == -8.0,
                    "compound material extent changed");
            require(shape.parts[0].transition &&
                    shape.parts[0].transition->motion.id == "frame-part-enter-1",
                    "compound part transition changed");
            require(shape.parts[0].transition->protrusion == 52.0,
                    "compound part protrusion changed");
            require(shape.parts[0].opacity == 0.75, "compound opacity changed");
            require(shape.parts[1].corners.bottomRight == 22.0, "compound corner radii changed");
            require(shape.connectors.size() == 1, "compound connectors changed");
            require(shape.connectorCurve == 7.0, "compound connector curve changed");
        }},
        Case{"compound corner authority is unambiguous", [] {
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
                    "shape":{"kind":"compound","parts":[{
                        "x":0,"y":0,"width":1000,"height":44,
                        "radius":22,
                        "corner_radii":{
                            "top_left":22,"top_right":22,
                            "bottom_right":22,"bottom_left":22
                        }
                    }]}
                }]
            })");
            require(!result, "duplicate corner authorities were accepted");
            require(result.error().path == "targets[0].shape.parts[0]",
                    "corner-authority failure path changed");
        }},
        Case{"transition bounds are enforced", [] {
            const auto elapsed = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"preview","kind":"region",
                    "selector":{"output":"DP-1"},
                    "geometry":{"space":"output-logical","x":0,"y":0,"width":100,"height":100},
                    "stage":"post-windows",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":10},
                    "transition":{
                        "id":"bad-motion","phase":"exit","edge":"top",
                        "duration_ms":100,"elapsed_ms":101,"travel":20
                    }
                }]
            })");
            require(!elapsed, "elapsed transition beyond duration was accepted");
            require(elapsed.error().path == "targets[0].transition.elapsed_ms",
                    "elapsed transition failure path changed");

            const auto protrusion = parseRequest(R"({
                "version":2,"operation":"session.replace",
                "session_id":"one","token":"two","generation":1,
                "materials":{"fluid":{}},
                "targets":[{
                    "id":"preview","kind":"region",
                    "selector":{"output":"DP-1"},
                    "geometry":{"space":"output-logical","x":0,"y":0,"width":100,"height":100},
                    "stage":"post-windows",
                    "material":{"source":"session","name":"fluid"},
                    "shape":{"kind":"rounded-rect","radius":10},
                    "transition":{
                        "id":"bad-motion","phase":"exit","edge":"top",
                        "duration_ms":100,"travel":20,"protrusion":20
                    }
                }]
            })");
            require(!protrusion, "part-only transition field was accepted on a target");
            require(protrusion.error().path == "targets[0].transition.protrusion",
                    "target transition authority failure path changed");
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
