#include "TestHarness.hpp"

#include "v2/targets/WindowAttachmentPlan.hpp"

#include <array>
#include <vector>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

WindowAttachmentState state(
    std::string owner,
    std::string targetId,
    std::uint64_t objectToken) {
    return {
        .identity = {
            .owner = std::move(owner),
            .targetId = std::move(targetId),
        },
        .objectToken = objectToken,
    };
}

} // namespace

int main() {
    return hfg::test::run({
        Case{"unchanged attachment is retained", [] {
            const std::array current{state("config", "files", 10)};
            const std::array desired{state("config", "files", 10)};
            const auto result =
                planWindowAttachments(current, desired);
            require(result.hasValue(), "valid plan failed");
            require(result.value().retain == std::vector{desired.front()}, "attachment was not retained");
            require(result.value().add.empty(), "retained attachment was re-added");
            require(result.value().remove.empty(), "retained attachment was removed");
        }},
        Case{"object replacement adds before removing old identity", [] {
            const std::array current{state("config", "files", 10)};
            const std::array desired{state("config", "files", 11)};
            const auto result =
                planWindowAttachments(current, desired);
            require(result.hasValue(), "replacement plan failed");
            require(result.value().retain.empty(), "replacement was retained");
            require(result.value().add == std::vector{desired.front()}, "replacement was not added");
            require(result.value().remove == std::vector{current.front()}, "old object was not removed");
        }},
        Case{"owner precedence replacement changes identity", [] {
            const std::array current{state("config", "files", 10)};
            const std::array desired{state("preview:lab:s1", "preview", 10)};
            const auto result =
                planWindowAttachments(current, desired);
            require(result.hasValue(), "precedence replacement failed");
            require(result.value().add == std::vector{desired.front()}, "new owner was not added");
            require(result.value().remove == std::vector{current.front()}, "old owner was not removed");
        }},
        Case{"new and vanished attachments are separated", [] {
            const std::array current{
                state("config", "files", 10),
                state("config", "terminal", 20),
            };
            const std::array desired{
                state("config", "terminal", 20),
                state("client:shell:s1", "bar", 30),
            };
            const auto result =
                planWindowAttachments(current, desired);
            require(result.hasValue(), "mixed plan failed");
            require(result.value().retain == std::vector{desired[0]}, "stable target was not retained");
            require(result.value().add == std::vector{desired[1]}, "new target was not added");
            require(result.value().remove == std::vector{current[0]}, "vanished target was not removed");
        }},
        Case{"two targets cannot own one window", [] {
            const std::array desired{
                state("config", "files", 10),
                state("client:shell:s1", "override", 10),
            };
            const auto result =
                planWindowAttachments({}, desired);
            require(!result, "duplicate object ownership was accepted");
            require(result.error().code == ErrorCode::InvalidRequest, "wrong duplicate-owner code");
            require(result.error().path == "desired[1].object_token", "wrong duplicate-owner path");
        }},
        Case{"identities and object tokens must be valid", [] {
            const std::array emptyIdentity{
                state("", "files", 10),
            };
            require(
                !planWindowAttachments({}, emptyIdentity),
                "empty owner was accepted");

            const std::array zeroToken{
                state("config", "files", 0),
            };
            require(
                !planWindowAttachments({}, zeroToken),
                "zero object token was accepted");
        }},
        Case{"duplicate identities are rejected", [] {
            const std::array desired{
                state("config", "files", 10),
                state("config", "files", 11),
            };
            const auto result =
                planWindowAttachments({}, desired);
            require(!result, "duplicate identity was accepted");
            require(result.error().path == "desired[1].identity", "wrong duplicate-identity path");
        }},
    });
}
