#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/core/Result.hpp"

#include <string>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

int main() {
    return hfg::test::run({
        Case{"value result", [] {
            auto result = Result<std::string>::success("ready");
            require(result.hasValue(), "success must hold a value");
            require(result.value() == "ready", "success value must survive");
        }},
        Case{"error result", [] {
            const Error expected{
                .code = ErrorCode::InvalidTarget,
                .path = "targets[0]",
                .message = "bad target",
            };
            auto result = Result<std::string>::failure(expected);
            require(!result, "failure must not hold a value");
            require(result.error() == expected, "failure must preserve its error");
        }},
        Case{"void success", [] {
            require(Result<void>::success().hasValue(), "void success must be successful");
        }},
        Case{"void failure", [] {
            auto result = Result<void>::failure({
                .code = ErrorCode::ResourceLimited,
                .path = "targets",
                .message = "too many",
            });
            require(!result, "void failure must fail");
            require(result.error().code == ErrorCode::ResourceLimited, "void failure must preserve its code");
        }},
        Case{"stable error names", [] {
            require(errorCodeName(ErrorCode::InvalidJson) == "invalid-json", "invalid-json name changed");
            require(errorCodeName(ErrorCode::StaleGeneration) == "stale-generation", "stale-generation name changed");
            require(errorCodeName(ErrorCode::InternalError) == "internal-error", "internal-error name changed");
        }},
        Case{"contract limits", [] {
            require(Limits::MAX_REQUEST_BYTES == 262'144U, "request limit changed");
            require(Limits::MAX_SESSIONS == 64U, "session limit changed");
            require(Limits::MAX_TARGETS_PER_SESSION == 256U, "per-session target limit changed");
            require(Limits::MAX_DYNAMIC_TARGETS == 512U, "dynamic target limit changed");
            require(Limits::MAX_RULES_PER_KIND == 512U, "rule limit changed");
            require(Limits::PREVIEW_LEASE_MS < Limits::CLIENT_LEASE_MS, "preview leases must be shorter");
        }},
    });
}
