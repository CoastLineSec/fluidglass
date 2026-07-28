#pragma once

#include "v2/model/Readiness.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace hfg::v2 {

template <typename Resource>
class PresentationResourceCache {
public:
  [[nodiscard]] std::shared_ptr<Resource>
  resourceFor(const PresentationKey &key) {
    auto [entry, inserted] = m_resources.try_emplace(key);
    if (inserted || !entry->second)
      entry->second = std::make_shared<Resource>();
    return entry->second;
  }

  void retain(const std::vector<PresentationKey> &keys) {
    std::erase_if(m_resources, [&keys](const auto &entry) {
      return std::ranges::find(keys, entry.first) == keys.end();
    });
  }

  void clear() noexcept { m_resources.clear(); }

  [[nodiscard]] std::size_t size() const noexcept {
    return m_resources.size();
  }

private:
  std::map<PresentationKey, std::shared_ptr<Resource>> m_resources;
};

} // namespace hfg::v2
