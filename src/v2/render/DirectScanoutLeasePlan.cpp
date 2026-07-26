#include "v2/render/DirectScanoutLeasePlan.hpp"

#include "v2/core/Limits.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace hfg::v2 {
namespace {

using LeaseMap = std::map<std::string, DirectScanoutLease, std::less<>>;

Result<LeaseMap> indexLeases(std::span<const DirectScanoutLease> leases,
                             std::string path) {
  if (leases.size() > Limits::MAX_COMPOSITOR_OBJECTS)
    return Result<LeaseMap>::failure({
        .code = ErrorCode::ResourceLimited,
        .path = std::move(path),
        .message = "direct-scanout lease count exceeds the supported limit",
    });

  LeaseMap indexed;
  std::set<std::uint64_t> objectTokens;
  for (std::size_t index = 0; index < leases.size(); ++index) {
    const auto &lease = leases[index];
    const auto itemPath = path + "[" + std::to_string(index) + "]";
    if (lease.output.empty())
      return Result<LeaseMap>::failure({
          .code = ErrorCode::InvalidRequest,
          .path = itemPath + ".output",
          .message = "direct-scanout lease output must not be empty",
      });
    if (lease.objectToken == 0U)
      return Result<LeaseMap>::failure({
          .code = ErrorCode::InvalidRequest,
          .path = itemPath + ".object_token",
          .message = "direct-scanout lease object token must not be zero",
      });
    if (!indexed.emplace(lease.output, lease).second)
      return Result<LeaseMap>::failure({
          .code = ErrorCode::InvalidRequest,
          .path = itemPath + ".output",
          .message = "direct-scanout lease outputs must be unique",
      });
    if (!objectTokens.insert(lease.objectToken).second)
      return Result<LeaseMap>::failure({
          .code = ErrorCode::InvalidRequest,
          .path = itemPath + ".object_token",
          .message = "direct-scanout lease object tokens must be unique",
      });
  }
  return Result<LeaseMap>::success(std::move(indexed));
}

} // namespace

Result<DirectScanoutLeasePlan>
planDirectScanoutLeases(std::span<const DirectScanoutLease> current,
                        std::span<const DirectScanoutLease> desired) {
  auto currentByOutput = indexLeases(current, "current");
  if (!currentByOutput)
    return Result<DirectScanoutLeasePlan>::failure(currentByOutput.error());
  auto desiredByOutput = indexLeases(desired, "desired");
  if (!desiredByOutput)
    return Result<DirectScanoutLeasePlan>::failure(desiredByOutput.error());

  DirectScanoutLeasePlan plan;
  for (const auto &[output, desiredLease] : desiredByOutput.value()) {
    const auto existing = currentByOutput.value().find(output);
    if (existing != currentByOutput.value().end() &&
        existing->second.objectToken == desiredLease.objectToken)
      plan.retain.push_back(desiredLease);
    else
      plan.acquire.push_back(desiredLease);
  }
  for (const auto &[output, currentLease] : currentByOutput.value()) {
    const auto wanted = desiredByOutput.value().find(output);
    if (wanted == desiredByOutput.value().end() ||
        wanted->second.objectToken != currentLease.objectToken)
      plan.release.push_back(currentLease);
  }
  return Result<DirectScanoutLeasePlan>::success(std::move(plan));
}

} // namespace hfg::v2
