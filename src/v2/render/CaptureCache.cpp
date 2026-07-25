#include "v2/render/CaptureCache.hpp"

#include "v2/core/Limits.hpp"

#include <algorithm>
#include <utility>

namespace hfg::v2 {

Result<void> CaptureResourceIndex::add(CaptureResource resource) {
    if (resource.token == 0U)
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "resource.token",
            "capture resource token must not be zero",
        });
    if (auto validation = validateCapturePlan(resource.plan); !validation)
        return Result<void>::failure({
            validation.error().code,
            "resource." + validation.error().path,
            validation.error().message,
        });
    if (m_resources.size() >= Limits::MAX_CAPTURE_REQUESTS)
        return Result<void>::failure({
            ErrorCode::ResourceLimited,
            "resources",
            "capture resource index is full",
        });
    if (std::ranges::any_of(m_resources, [&](const auto& existing) {
            return existing.token == resource.token;
        }))
        return Result<void>::failure({
            ErrorCode::InvalidRequest,
            "resource.token",
            "capture resource token already exists",
        });
    m_resources.emplace_back(std::move(resource));
    return Result<void>::success();
}

std::optional<CaptureResource> CaptureResourceIndex::findCovering(
    const CapturePlan& required) const {
    const CaptureResource* best = nullptr;
    for (const auto& resource : m_resources) {
        if (!capturePlanCovers(resource.plan, required))
            continue;
        if (!best || resource.plan.pixelCount < best->plan.pixelCount)
            best = &resource;
    }
    if (!best)
        return std::nullopt;
    return *best;
}

std::optional<CaptureResource> CaptureResourceIndex::remove(
    std::uint64_t token) {
    const auto entry = std::ranges::find_if(m_resources, [&](const auto& resource) {
        return resource.token == token;
    });
    if (entry == m_resources.end())
        return std::nullopt;
    auto removed = std::move(*entry);
    m_resources.erase(entry);
    return removed;
}

std::vector<CaptureResource> CaptureResourceIndex::retireGeneration(
    std::string_view output,
    std::uint64_t generation) {
    std::vector<CaptureResource> retired;
    std::erase_if(m_resources, [&](CaptureResource& resource) {
        if (resource.plan.key.output != output ||
            resource.plan.key.outputGeneration != generation)
            return false;
        retired.emplace_back(std::move(resource));
        return true;
    });
    return retired;
}

std::vector<CaptureResource> CaptureResourceIndex::retireOutput(
    std::string_view output) {
    std::vector<CaptureResource> retired;
    std::erase_if(m_resources, [&](CaptureResource& resource) {
        if (resource.plan.key.output != output)
            return false;
        retired.emplace_back(std::move(resource));
        return true;
    });
    return retired;
}

std::vector<CaptureResource> CaptureResourceIndex::clear() {
    auto retired = std::move(m_resources);
    m_resources.clear();
    return retired;
}

std::size_t CaptureResourceIndex::size() const noexcept {
    return m_resources.size();
}

} // namespace hfg::v2
