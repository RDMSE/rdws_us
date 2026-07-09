#include "DeviceConfigService.h"

namespace rdws::device_config {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;

std::optional<DeviceConfig> DeviceConfigService::findByDeviceId(const std::string& deviceId) {
  return repo_.findByDeviceId(deviceId);
}

std::string DeviceConfigService::create(const DeviceConfigCreate& data) {
  return repo_.create(data);
}

OperationResult DeviceConfigService::update(const std::string& deviceId,
                                            const DeviceConfigUpdate& data) {
  if (!repo_.findByDeviceId(deviceId)) {
    return OperationResult::error("Device config not found for device_id " + deviceId, 404);
  }
  // execCommand só reflete falha em erro de SQL/conexão — não em "0 linhas afetadas" —
  // por isso a checagem de existência acima é o que garante o 404 correto.
  if (!repo_.update(deviceId, data)) {
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
