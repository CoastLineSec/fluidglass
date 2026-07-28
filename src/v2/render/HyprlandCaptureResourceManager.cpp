#include "v2/render/HyprlandCaptureResourceManager.hpp"

#include "v2/render/CaptureResourcePlan.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

template <typename T>
Result<T> failure(
    ErrorCode code,
    std::string path,
    std::string message) {
    return Result<T>::failure({
        .code = code,
        .path = std::move(path),
        .message = std::move(message),
    });
}

Error allocationError(
    std::size_t captureIndex,
    const Error& source) {
    return {
        .code = source.code,
        .path = "captures[" +
            std::to_string(captureIndex) + "]." +
            source.path,
        .message = source.message,
    };
}

} // namespace

Result<CaptureResourceReconcileResult>
HyprlandCaptureResourceManager::reconcile(
    std::span<const CapturePlan> desired,
    std::uint64_t maxTotalBytes) {
    auto resourcePlan = planCaptureResources(
        resources(),
        desired,
        maxTotalBytes);
    if (!resourcePlan)
        return Result<CaptureResourceReconcileResult>::failure(
            resourcePlan.error());

    const auto allocationCount =
        resourcePlan.value().allocate.size();
    if (allocationCount >
        std::numeric_limits<std::uint64_t>::max() -
            m_lastToken)
        return failure<CaptureResourceReconcileResult>(
            ErrorCode::ResourceLimited,
            "resource_token",
            "capture resource token space is exhausted");

    CaptureResourceReconcileResult result;
    result.retiredTokens.reserve(
        resourcePlan.value().retire.size());
    for (const auto& retired :
         resourcePlan.value().retire)
        result.retiredTokens.push_back(retired.token);

    if (!resourcePlan.value().allocateBeforeRetire)
        retire(resourcePlan.value().retire);

    std::vector<std::optional<std::uint64_t>>
        allocationTokens(allocationCount);
    for (std::size_t allocationIndex = 0;
         allocationIndex < allocationCount;
         ++allocationIndex) {
        const auto& capture =
            resourcePlan.value().allocate[allocationIndex];
        const auto binding = std::ranges::find_if(
            resourcePlan.value().bindings,
            [&](const CaptureResourceBinding& candidate) {
                return candidate.allocationIndex ==
                    allocationIndex;
            });
        if (binding ==
            resourcePlan.value().bindings.end())
            return failure<CaptureResourceReconcileResult>(
                ErrorCode::InternalError,
                "bindings",
                "capture allocation has no desired binding");

        auto token = nextToken();
        if (!token)
            return Result<CaptureResourceReconcileResult>::failure(
                token.error());
        auto allocated =
            HyprlandCaptureResource::allocate(capture);
        if (!allocated) {
            result.failures.push_back({
                .captureIndex = binding->captureIndex,
                .error = allocationError(
                    binding->captureIndex,
                    allocated.error()),
            });
            continue;
        }
        allocationTokens[allocationIndex] = token.value();
        m_entries.push_back({
            .token = token.value(),
            .resource = std::move(allocated.value()),
        });
    }

    if (resourcePlan.value().allocateBeforeRetire)
        retire(resourcePlan.value().retire);

    result.bindings.reserve(
        resourcePlan.value().bindings.size());
    for (const auto& binding :
         resourcePlan.value().bindings) {
        if (binding.retainedToken) {
            result.bindings.push_back({
                .captureIndex = binding.captureIndex,
                .resourceToken = *binding.retainedToken,
            });
            continue;
        }
        if (!binding.allocationIndex ||
            *binding.allocationIndex >=
                allocationTokens.size())
            return failure<CaptureResourceReconcileResult>(
                ErrorCode::InternalError,
                "bindings",
                "capture binding has no valid resource source");
        const auto token =
            allocationTokens[*binding.allocationIndex];
        if (!token)
            continue;
        result.bindings.push_back({
            .captureIndex = binding.captureIndex,
            .resourceToken = *token,
        });
    }
    result.resources = resources();
    return Result<CaptureResourceReconcileResult>::success(
        std::move(result));
}

void HyprlandCaptureResourceManager::clear() noexcept {
    m_entries.clear();
}

std::vector<CaptureResource>
HyprlandCaptureResourceManager::resources() const {
    std::vector<CaptureResource> result;
    result.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        if (!entry.resource ||
            !entry.resource->allocated())
            continue;
        result.push_back({
            .token = entry.token,
            .plan = entry.resource->plan(),
        });
    }
    return result;
}

const HyprlandCaptureResource*
HyprlandCaptureResourceManager::resourceFor(
    std::uint64_t token) const noexcept {
    const auto entry = std::ranges::find_if(
        m_entries,
        [&](const Entry& candidate) {
            return candidate.token == token &&
                candidate.resource &&
                candidate.resource->allocated();
        });
    if (entry == m_entries.end())
        return nullptr;
    return entry->resource.get();
}

HyprlandCaptureResource*
HyprlandCaptureResourceManager::resourceFor(
    std::uint64_t token) noexcept {
    const auto entry = std::ranges::find_if(
        m_entries,
        [&](const Entry& candidate) {
            return candidate.token == token &&
                candidate.resource &&
                candidate.resource->allocated();
        });
    if (entry == m_entries.end())
        return nullptr;
    return entry->resource.get();
}

Result<std::uint64_t>
HyprlandCaptureResourceManager::nextToken() {
    if (m_lastToken ==
        std::numeric_limits<std::uint64_t>::max())
        return failure<std::uint64_t>(
            ErrorCode::ResourceLimited,
            "resource_token",
            "capture resource token space is exhausted");
    return Result<std::uint64_t>::success(
        ++m_lastToken);
}

void HyprlandCaptureResourceManager::retire(
    std::span<const CaptureResource> resourcesToRetire) {
    for (const auto& resource : resourcesToRetire)
        std::erase_if(
            m_entries,
            [&](const Entry& entry) {
                return entry.token == resource.token;
            });
}

} // namespace hfg::v2
