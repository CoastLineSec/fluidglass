#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CaptureCache.hpp"
#include "v2/render/HyprlandCaptureResource.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hfg::v2 {

struct ManagedCaptureBinding {
    std::size_t   captureIndex = 0;
    std::uint64_t resourceToken = 0;

    friend bool operator==(
        const ManagedCaptureBinding&,
        const ManagedCaptureBinding&) = default;
};

struct CaptureAllocationFailure {
    std::size_t captureIndex = 0;
    Error       error;

    friend bool operator==(
        const CaptureAllocationFailure&,
        const CaptureAllocationFailure&) = default;
};

struct CaptureResourceReconcileResult {
    std::vector<ManagedCaptureBinding> bindings;
    std::vector<CaptureAllocationFailure> failures;
    std::vector<CaptureResource> resources;
    std::vector<std::uint64_t> retiredTokens;

    friend bool operator==(
        const CaptureResourceReconcileResult&,
        const CaptureResourceReconcileResult&) = default;
};

class HyprlandCaptureResourceManager {
  public:
    [[nodiscard]] Result<CaptureResourceReconcileResult>
    reconcile(
        std::span<const CapturePlan> desired,
        std::uint64_t maxTotalBytes,
        std::span<const std::uint64_t> retainedTokens = {});

    void clear() noexcept;

    [[nodiscard]] std::vector<CaptureResource>
    resources() const;
    [[nodiscard]] const HyprlandCaptureResource*
    resourceFor(std::uint64_t token) const noexcept;
    [[nodiscard]] HyprlandCaptureResource*
    resourceFor(std::uint64_t token) noexcept;

  private:
    struct Entry {
        std::uint64_t token = 0;
        std::unique_ptr<HyprlandCaptureResource> resource;
    };

    [[nodiscard]] Result<std::uint64_t> nextToken();
    void retire(std::span<const CaptureResource> resources);

    std::vector<Entry> m_entries;
    std::uint64_t      m_lastToken = 0;
};

} // namespace hfg::v2
