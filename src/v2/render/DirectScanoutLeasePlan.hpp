#pragma once

#include "v2/core/Result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hfg::v2 {

struct DirectScanoutLease {
  std::string output;
  std::uint64_t objectToken = 0;

  friend bool operator==(const DirectScanoutLease &,
                         const DirectScanoutLease &) = default;
  friend auto operator<=>(const DirectScanoutLease &,
                          const DirectScanoutLease &) = default;
};

struct DirectScanoutLeasePlan {
  std::vector<DirectScanoutLease> retain;
  std::vector<DirectScanoutLease> acquire;
  std::vector<DirectScanoutLease> release;

  friend bool operator==(const DirectScanoutLeasePlan &,
                         const DirectScanoutLeasePlan &) = default;
};

[[nodiscard]] Result<DirectScanoutLeasePlan>
planDirectScanoutLeases(std::span<const DirectScanoutLease> current,
                        std::span<const DirectScanoutLease> desired);

} // namespace hfg::v2
