#include "DeviceConfigService.h"

#include "../utils/json_merge.h"

namespace rdws::device_config {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
namespace json = rdws::utils::json;

std::optional<DeviceConfig> DeviceConfigService::findByDeviceId(const std::string& deviceId) {
  return repo_.findByDeviceId(deviceId);
}

std::string DeviceConfigService::create(const DeviceConfigCreate& data) {
  return repo_.create(data);
}

OperationResult DeviceConfigService::update(const std::string& deviceId,
                                            const DeviceConfigUpdate& data) {
  const auto existing = repo_.findByDeviceId(deviceId);
  if (!existing) {
    return OperationResult::error("Device config not found for device_id " + deviceId, 404);
  }

  const std::string mergedJson = json::mergePatch(existing->config, data.configJson);

  // execCommand only reflects failure in SQL/connection error — not in "0 rows affected" —
  // that's why the existence check above ensures the correct 404.
  if (!repo_.update(deviceId, {mergedJson})) {
    return OperationResult::error("Failed to update device config", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

OperationResult DeviceConfigService::remove(const std::string& deviceId) {
  if (!repo_.findByDeviceId(deviceId)) {
    return OperationResult::error("Device config not found for device_id " + deviceId, 404);
  }
  if (!repo_.remove(deviceId)) {
    return OperationResult::error("Failed to delete device config", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Deleted"});
}

} // namespace rdws::device_config
