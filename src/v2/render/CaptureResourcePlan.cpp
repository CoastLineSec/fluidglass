#include "v2/render/CaptureResourcePlan.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

Result<CaptureResourcePlan> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<CaptureResourcePlan>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

bool addWithoutOverflow(
    std::uint64_t& total,
    std::uint64_t value) {
    if (value >
        std::numeric_limits<std::uint64_t>::max() - total)
        return false;
    total += value;
    return true;
}

Result<void> validateInputs(
    std::span<const CaptureResource> current,
    std::span<const CapturePlan> desired) {
    if (current.size() > Limits::MAX_CAPTURE_REQUESTS)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "current",
            "capture resource count exceeds the supported limit",
        });
    if (desired.size() > Limits::MAX_CAPTURE_REQUESTS)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "desired",
            "desired capture count exceeds the supported limit",
        });

    std::set<std::uint64_t> resourceTokens;
    for (std::size_t index = 0; index < current.size(); ++index) {
        const auto& resource = current[index];
        const auto path =
            "current[" + std::to_string(index) + "]";
        if (resource.token == 0U)
            return Result<void>::failure({
                ErrorCode::InvalidRequest,
                path + ".token",
                "capture resource token must not be zero",
            });
        if (!resourceTokens.insert(resource.token).second)
            return Result<void>::failure({
                ErrorCode::InvalidRequest,
                path + ".token",
                "capture resource tokens must be unique",
            });
        if (auto validation =
                validateCapturePlan(resource.plan);
            !validation)
            return Result<void>::failure({
                validation.error().code,
                path + "." + validation.error().path,
                validation.error().message,
            });
    }

    for (std::size_t index = 0; index < desired.size(); ++index) {
        const auto path =
            "desired[" + std::to_string(index) + "]";
        if (auto validation =
                validateCapturePlan(desired[index]);
            !validation)
            return Result<void>::failure({
                validation.error().code,
                path + "." + validation.error().path,
                validation.error().message,
            });
        for (std::size_t previous = 0;
             previous < index;
             ++previous) {
            if (desired[previous] == desired[index])
                return Result<void>::failure({
                    ErrorCode::InvalidRequest,
                    path,
                    "desired capture plans must be unique",
                });
        }
    }
    return Result<void>::success();
}

} // namespace

Result<CaptureResourcePlan>
planCaptureResources(
    std::span<const CaptureResource> current,
    std::span<const CapturePlan> desired,
    std::uint64_t maxTotalBytes) {
    if (maxTotalBytes == 0U)
        return failure(
            ErrorCode::ResourceLimited,
            "max_total_bytes",
            "total capture byte budget must not be zero");
    if (auto validation = validateInputs(current, desired);
        !validation)
        return Result<CaptureResourcePlan>::failure(
            validation.error());

    std::uint64_t baselineBytes = 0;
    for (const auto& capture : desired) {
        if (!addWithoutOverflow(
                baselineBytes,
                capture.byteCount))
            return failure(
                ErrorCode::ResourceLimited,
                "desired",
                "desired capture byte total overflows");
    }
    if (baselineBytes > maxTotalBytes)
        return failure(
            ErrorCode::ResourceLimited,
            "desired",
            "desired captures exceed the total capture byte budget");

    std::vector<std::optional<std::uint64_t>>
        retainedByCapture(desired.size());
    std::set<std::uint64_t> selectedTokens;
    auto projectedBytes = baselineBytes;

    while (true) {
        struct Candidate {
            std::size_t              resourceIndex = 0;
            std::vector<std::size_t> covered;
            std::uint64_t            resultingBytes = 0;
        };
        std::optional<Candidate> best;

        for (std::size_t resourceIndex = 0;
             resourceIndex < current.size();
             ++resourceIndex) {
            const auto& resource = current[resourceIndex];
            if (selectedTokens.contains(resource.token))
                continue;

            Candidate candidate{
                .resourceIndex = resourceIndex,
                .covered = {},
                .resultingBytes = projectedBytes,
            };
            std::uint64_t replacedBytes = 0;
            for (std::size_t captureIndex = 0;
                 captureIndex < desired.size();
                 ++captureIndex) {
                if (retainedByCapture[captureIndex] ||
                    !capturePlanCovers(
                        resource.plan,
                        desired[captureIndex]))
                    continue;
                candidate.covered.push_back(captureIndex);
                if (!addWithoutOverflow(
                        replacedBytes,
                        desired[captureIndex].byteCount))
                    return failure(
                        ErrorCode::ResourceLimited,
                        "desired",
                        "covered capture byte total overflows");
            }
            if (candidate.covered.empty())
                continue;

            candidate.resultingBytes -= replacedBytes;
            if (!addWithoutOverflow(
                    candidate.resultingBytes,
                    resource.plan.byteCount) ||
                candidate.resultingBytes > maxTotalBytes)
                continue;

            if (!best ||
                candidate.resultingBytes <
                    best->resultingBytes ||
                (candidate.resultingBytes ==
                        best->resultingBytes &&
                    candidate.covered.size() >
                        best->covered.size()) ||
                (candidate.resultingBytes ==
                        best->resultingBytes &&
                    candidate.covered.size() ==
                        best->covered.size() &&
                    resource.plan.byteCount <
                        current[best->resourceIndex]
                            .plan.byteCount) ||
                (candidate.resultingBytes ==
                        best->resultingBytes &&
                    candidate.covered.size() ==
                        best->covered.size() &&
                    resource.plan.byteCount ==
                        current[best->resourceIndex]
                            .plan.byteCount &&
                    resource.token <
                        current[best->resourceIndex].token))
                best = std::move(candidate);
        }

        if (!best)
            break;
        const auto token =
            current[best->resourceIndex].token;
        selectedTokens.insert(token);
        projectedBytes = best->resultingBytes;
        for (const auto captureIndex : best->covered)
            retainedByCapture[captureIndex] = token;
    }

    CaptureResourcePlan plan{
        .retain = {},
        .allocate = {},
        .retire = {},
        .bindings = {},
        .totalBytes = projectedBytes,
    };
    for (const auto& resource : current) {
        if (selectedTokens.contains(resource.token))
            plan.retain.push_back(resource);
        else
            plan.retire.push_back(resource);
    }
    plan.bindings.reserve(desired.size());
    for (std::size_t captureIndex = 0;
         captureIndex < desired.size();
         ++captureIndex) {
        if (retainedByCapture[captureIndex]) {
            plan.bindings.push_back({
                .captureIndex = captureIndex,
                .retainedToken =
                    retainedByCapture[captureIndex],
                .allocationIndex = std::nullopt,
            });
            continue;
        }
        const auto allocationIndex = plan.allocate.size();
        plan.allocate.push_back(desired[captureIndex]);
        plan.bindings.push_back({
            .captureIndex = captureIndex,
            .retainedToken = std::nullopt,
            .allocationIndex = allocationIndex,
        });
    }
    return Result<CaptureResourcePlan>::success(
        std::move(plan));
}

} // namespace hfg::v2
