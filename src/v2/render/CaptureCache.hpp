#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/CapturePlan.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hfg::v2 {

struct CaptureResource {
    std::uint64_t token = 0;
    CapturePlan   plan;

    friend bool operator==(const CaptureResource&, const CaptureResource&) = default;
};

class CaptureResourceIndex {
  public:
    [[nodiscard]] Result<void> add(CaptureResource resource);
    [[nodiscard]] std::optional<CaptureResource> findCovering(
        const CapturePlan& required) const;
    [[nodiscard]] std::optional<CaptureResource> remove(std::uint64_t token);
    [[nodiscard]] std::vector<CaptureResource> retireGeneration(
        std::string_view output,
        std::uint64_t generation);
    [[nodiscard]] std::vector<CaptureResource> retireOutput(
        std::string_view output);
    [[nodiscard]] std::vector<CaptureResource> clear();
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<CaptureResource> m_resources;
};

} // namespace hfg::v2
