#pragma once

#include "v2/core/Result.hpp"
#include "v2/model/Session.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>

namespace hfg::v2 {

struct CapabilitiesRequest {};
struct StatusRequest {};

struct OpenSessionRequest {
    std::string clientId;
    SessionMode mode = SessionMode::Client;
};

struct ReplaceSessionRequest {
    std::string        sessionId;
    std::string        token;
    SessionReplacement replacement;
};

struct HeartbeatSessionRequest {
    std::string   sessionId;
    std::string   token;
    std::uint64_t generation = 0;
};

struct CloseSessionRequest {
    std::string sessionId;
    std::string token;
};

struct InspectTargetRequest {
    std::string sessionId;
    std::string token;
    std::string targetId;
};

using RequestBody = std::variant<
    CapabilitiesRequest,
    StatusRequest,
    OpenSessionRequest,
    ReplaceSessionRequest,
    HeartbeatSessionRequest,
    CloseSessionRequest,
    InspectTargetRequest>;

struct Request {
    std::optional<std::string> requestId;
    RequestBody                body;
};

[[nodiscard]] Result<Request> parseRequest(std::string_view payload);

[[nodiscard]] std::string successResponse(
    const std::optional<std::string>& requestId,
    const nlohmann::json& result);

[[nodiscard]] std::string failureResponse(
    const std::optional<std::string>& requestId,
    const Error& error);

} // namespace hfg::v2
