#include "TestHarness.hpp"

#include "v2/core/Limits.hpp"
#include "v2/model/Session.hpp"

#include <cstdint>
#include <set>
#include <string>

using hfg::test::Case;
using hfg::test::require;
using namespace hfg::v2;

namespace {

class IdSequence {
  public:
    std::string next() {
        return "opaque-" + std::to_string(++m_value);
    }

  private:
    std::uint64_t m_value = 0;
};

Material material(std::string name) {
    auto result = validateMaterial(std::move(name), {});
    if (!result)
        throw hfg::test::Failure("test material failed validation");
    return result.value();
}

Target target(std::string id, MaterialSource source, std::string materialName) {
    auto result = validateTarget({
        .id = std::move(id),
        .kind = TargetKind::Region,
        .material = {.source = source, .name = std::move(materialName)},
        .shape = RoundedRectShape{.radius = 10.0},
        .selector = RegionSelector{.output = "DP-1"},
        .geometry = Rect{.x = 0.0, .y = 0.0, .width = 100.0, .height = 80.0},
        .stage = RenderStage::PostWindows,
    });
    if (!result)
        throw hfg::test::Failure("test target failed validation");
    return result.value();
}

struct Fixture {
    IdSequence ids;
    SessionManager manager{[this] {
        return ids.next();
    }};
};

} // namespace

int main() {
    return hfg::test::run({
        Case{"open client and preview leases", [] {
            Fixture fixture;
            const auto client = fixture.manager.open("shell", SessionMode::Client, 100);
            const auto preview = fixture.manager.open("settings", SessionMode::Preview, 100);
            require(client && preview, "sessions must open");
            require(client.value().leaseMs == Limits::CLIENT_LEASE_MS, "client lease changed");
            require(preview.value().leaseMs == Limits::PREVIEW_LEASE_MS, "preview lease changed");
            require(client.value().token != preview.value().token, "tokens must be distinct");
        }},
        Case{"invalid client id", [] {
            Fixture fixture;
            require(!fixture.manager.open("bad/client", SessionMode::Client, 0), "invalid client id must fail");
        }},
        Case{"atomic replacement", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 1,
                .materials = {{"glass", material("glass")}},
                .targets = {target("bar", MaterialSource::Session, "glass")},
            };
            const auto result = fixture.manager.replace(
                handle.sessionId, handle.token, std::move(replacement), {}, 100);
            require(result.hasValue(), "valid replacement must succeed");
            require(result.value().generation == 1, "generation did not advance");
            require(fixture.manager.targetCount() == 1, "target was not installed");

            SessionReplacement invalid{
                .generation = 2,
                .materials = {},
                .targets = {target("broken", MaterialSource::Session, "missing")},
            };
            require(!fixture.manager.replace(handle.sessionId, handle.token, std::move(invalid), {}, 200),
                    "missing material must reject replacement");
            const auto snapshot = fixture.manager.snapshot(handle.sessionId);
            require(snapshot && snapshot->generation == 1, "failed replacement changed generation");
            require(snapshot->targets.size() == 1 && snapshot->targets[0].id == "bar",
                    "failed replacement changed live targets");
        }},
        Case{"config material reference", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 1,
                .materials = {},
                .targets = {target("bar", MaterialSource::Config, "global")},
            };
            require(fixture.manager.replace(handle.sessionId, handle.token, replacement, {"global"}, 1).hasValue(),
                    "known config material must resolve");
            replacement.generation = 2;
            replacement.targets[0] = target("bar", MaterialSource::Config, "missing");
            require(!fixture.manager.replace(handle.sessionId, handle.token, replacement, {"global"}, 2),
                    "unknown config material must fail");
        }},
        Case{"generation and token enforcement", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 2,
                .materials = {},
                .targets = {},
            };
            auto result = fixture.manager.replace(handle.sessionId, handle.token, replacement, {}, 1);
            require(!result && result.error().code == ErrorCode::StaleGeneration, "skipped generation must fail");
            replacement.generation = 1;
            result = fixture.manager.replace(handle.sessionId, "wrong", replacement, {}, 1);
            require(!result && result.error().code == ErrorCode::InvalidToken, "wrong token must fail");
        }},
        Case{"heartbeat renews matching generation", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 100).value();
            const auto renewed = fixture.manager.heartbeat(handle.sessionId, handle.token, 0, 1'000);
            require(renewed.hasValue(), "valid heartbeat must succeed");
            require(renewed.value().expiresAtMs == 1'000 + Limits::CLIENT_LEASE_MS, "lease was not renewed");
            require(!fixture.manager.heartbeat(handle.sessionId, handle.token, 1, 1'001), "wrong generation must fail");
        }},
        Case{"inspection authenticates without renewing", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 100).value();
            const auto inspected = fixture.manager.inspect(handle.sessionId, handle.token, 1'000);
            require(inspected.hasValue(), "valid inspection must succeed");
            require(inspected.value().expiresAtMs == handle.expiresAtMs, "inspection renewed the lease");
            const auto wrongToken = fixture.manager.inspect(handle.sessionId, "wrong", 1'001);
            require(!wrongToken && wrongToken.error().code == ErrorCode::InvalidToken,
                    "inspection accepted a wrong token");
        }},
        Case{"expiry removes only expired owner", [] {
            Fixture fixture;
            const auto preview = fixture.manager.open("settings", SessionMode::Preview, 0).value();
            const auto client = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 1,
                .materials = {{"glass", material("glass")}},
                .targets = {target("preview", MaterialSource::Session, "glass")},
            };
            require(fixture.manager.replace(
                preview.sessionId,
                preview.token,
                std::move(replacement),
                {},
                0).hasValue(), "preview replacement failed");
            const auto expired = fixture.manager.expire(Limits::PREVIEW_LEASE_MS);
            require(expired.size() == 1, "one preview should expire");
            require(expired[0].owner.starts_with("preview:settings:"), "expired owner identity is wrong");
            require(expired[0].targetIds == std::vector<std::string>{"preview"},
                    "expired target identities were lost");
            require(!fixture.manager.snapshot(preview.sessionId), "expired preview remained live");
            require(fixture.manager.snapshot(client.sessionId).has_value(), "client expired too early");
        }},
        Case{"snapshot enumeration excludes tokens", [] {
            Fixture fixture;
            const auto alpha = fixture.manager.open("alpha", SessionMode::Client, 0);
            const auto beta = fixture.manager.open("beta", SessionMode::Preview, 0);
            require(alpha && beta, "sessions must open");
            const auto snapshots = fixture.manager.snapshots();
            require(snapshots.size() == 2, "snapshot enumeration lost sessions");
            require(snapshots[0].owner.starts_with("client:alpha:"), "client owner is wrong");
            require(snapshots[1].owner.starts_with("preview:beta:"), "preview owner is wrong");
        }},
        Case{"close is briefly idempotent", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            require(fixture.manager.close(handle.sessionId, handle.token, 100).hasValue(), "first close must succeed");
            require(fixture.manager.close(handle.sessionId, handle.token, 101).hasValue(), "repeated close must succeed");
            require(!fixture.manager.close(handle.sessionId, "wrong", 102), "wrong tombstone token must fail");
            require(!fixture.manager.close(
                handle.sessionId, handle.token, 100 + Limits::SESSION_TOMBSTONE_MS),
                "expired tombstone must not authorize close");
        }},
        Case{"duplicate target ids reject atomically", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 1,
                .materials = {{"glass", material("glass")}},
                .targets = {
                    target("same", MaterialSource::Session, "glass"),
                    target("same", MaterialSource::Session, "glass"),
                },
            };
            require(!fixture.manager.replace(handle.sessionId, handle.token, replacement, {}, 1),
                    "duplicate target ids must fail");
            require(fixture.manager.targetCount() == 0, "failed duplicate replacement changed state");
        }},
        Case{"material key must match name", [] {
            Fixture fixture;
            const auto handle = fixture.manager.open("shell", SessionMode::Client, 0).value();
            SessionReplacement replacement{
                .generation = 1,
                .materials = {{"alias", material("actual")}},
                .targets = {},
            };
            require(!fixture.manager.replace(handle.sessionId, handle.token, replacement, {}, 1),
                    "material alias must fail");
        }},
    });
}
