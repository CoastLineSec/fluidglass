#include "v2/render/HyprlandDirectScanoutInhibitor.hpp"

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace hfg::v2 {
namespace {

Result<void> failure(ErrorCode code, std::string path, std::string message) {
  return Result<void>::failure({
      .code = code,
      .path = std::move(path),
      .message = std::move(message),
  });
}

} // namespace

HyprlandDirectScanoutInhibitor::HyprlandDirectScanoutInhibitor(
    HyprlandOutputCatalog &outputs)
    : m_outputs(outputs) {}

HyprlandDirectScanoutInhibitor::~HyprlandDirectScanoutInhibitor() { clear(); }

Result<void> HyprlandDirectScanoutInhibitor::reconcile(
    std::span<const DirectScanoutLease> desired) {
  // lock(), never operator bool: the weak reference's boolean test does not
  // prove the monitor is still alive, only that the slot is engaged.
  //
  // Load-bearing ordering: this guard only permits reconciliation because
  // Hyprland emits render.preChecks BEFORE assigning m_renderData.pMonitor
  // for the frame. If a Hyprland release ever moves preChecks inside the
  // frame, every reconcile fails here and glass tears down at refresh rate —
  // loudly, which is the intended failure mode for that upstream change.
  if (g_pHyprRenderer && g_pHyprRenderer->m_renderData.pMonitor.lock())
    return failure(
        ErrorCode::UnsupportedOperation, "direct-scanout.render-frame",
        "direct-scanout leases cannot change during an active render frame");

  auto &pointerManager = Pointer::mgr();
  if (!pointerManager)
    return failure(ErrorCode::UnsupportedOperation, "pointer-manager",
                   "Hyprland pointer manager is unavailable");

  pruneExpired();
  auto plan = planDirectScanoutLeases(leases(), desired);
  if (!plan)
    return Result<void>::failure(plan.error());

  std::vector<Entry> provisional;
  provisional.reserve(plan.value().acquire.size());
  try {
    for (const auto &lease : plan.value().acquire) {
      auto monitor = m_outputs.monitorFor(lease.objectToken);
      if (!monitor) {
        for (auto &entry : provisional)
          release(entry);
        return Result<void>::failure(monitor.error());
      }
      if (monitor.value()->m_name != lease.output) {
        for (auto &entry : provisional)
          release(entry);
        return failure(ErrorCode::StaleGeneration, "output",
                       "direct-scanout lease output identity changed");
      }
      pointerManager->lockSoftwareForMonitor(monitor.value());
      provisional.push_back({
          .lease = lease,
          .monitor = monitor.value(),
      });
    }
  } catch (const std::exception &error) {
    for (auto &entry : provisional)
      release(entry);
    return failure(ErrorCode::InternalError, "direct-scanout", error.what());
  } catch (...) {
    for (auto &entry : provisional)
      release(entry);
    return failure(ErrorCode::InternalError, "direct-scanout",
                   "failed to acquire a compositor render lease");
  }

  for (const auto &lease : plan.value().release) {
    const auto existing = std::ranges::find_if(
        m_entries, [&](const Entry &entry) { return entry.lease == lease; });
    if (existing == m_entries.end())
      continue;
    release(*existing);
    m_entries.erase(existing);
  }
  m_entries.insert(m_entries.end(),
                   std::make_move_iterator(provisional.begin()),
                   std::make_move_iterator(provisional.end()));
  return Result<void>::success();
}

std::vector<DirectScanoutLease> HyprlandDirectScanoutInhibitor::leases() const {
  std::vector<DirectScanoutLease> result;
  result.reserve(m_entries.size());
  for (const auto &entry : m_entries)
    if (!entry.monitor.expired())
      result.push_back(entry.lease);
  return result;
}

void HyprlandDirectScanoutInhibitor::clear() noexcept {
  for (auto &entry : m_entries)
    release(entry);
  m_entries.clear();
}

void HyprlandDirectScanoutInhibitor::pruneExpired() noexcept {
  std::erase_if(m_entries,
                [](const Entry &entry) { return entry.monitor.expired(); });
}

void HyprlandDirectScanoutInhibitor::release(Entry &entry) noexcept {
  const auto monitor = entry.monitor.lock();
  auto &pointerManager = Pointer::mgr();
  if (!monitor || !pointerManager)
    return;
  try {
    pointerManager->unlockSoftwareForMonitor(monitor);
  } catch (...) {
  }
}

} // namespace hfg::v2
