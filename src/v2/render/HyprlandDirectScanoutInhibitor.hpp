#pragma once

#include "v2/core/Result.hpp"
#include "v2/render/DirectScanoutLeasePlan.hpp"
#include "v2/render/HyprlandOutputCatalog.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <span>
#include <vector>

namespace hfg::v2 {

class HyprlandDirectScanoutInhibitor {
public:
  explicit HyprlandDirectScanoutInhibitor(HyprlandOutputCatalog &outputs);
  ~HyprlandDirectScanoutInhibitor();

  HyprlandDirectScanoutInhibitor(const HyprlandDirectScanoutInhibitor &) =
      delete;
  HyprlandDirectScanoutInhibitor &
  operator=(const HyprlandDirectScanoutInhibitor &) = delete;
  HyprlandDirectScanoutInhibitor(HyprlandDirectScanoutInhibitor &&) = delete;
  HyprlandDirectScanoutInhibitor &
  operator=(HyprlandDirectScanoutInhibitor &&) = delete;

  [[nodiscard]] Result<void>
  reconcile(std::span<const DirectScanoutLease> desired);

  [[nodiscard]] std::vector<DirectScanoutLease> leases() const;
  void clear() noexcept;

private:
  struct Entry {
    DirectScanoutLease lease;
    PHLMONITORREF monitor;
  };

  void pruneExpired() noexcept;
  void release(Entry &entry) noexcept;

  HyprlandOutputCatalog &m_outputs;
  std::vector<Entry> m_entries;
};

} // namespace hfg::v2
