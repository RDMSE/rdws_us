#include "DeviceConfigService.h"

namespace rdws::device_config {

std::optional<DeviceConfig> DeviceConfigService::findByDeviceId(const std::string& deviceId) {
  return repo_.findByDeviceId(deviceId);
}

std::string DeviceConfigService::create(const DeviceConfigCreate& data) {
  return repo_.create(data);
}

bool DeviceConfigService::update(const std::string& deviceId, const DeviceConfigUpdate& data) {
  return repo_.update(deviceId, data);
}

bool DeviceConfigService::remove(const std::string& deviceId) {
  return repo_.remove(deviceId);
}

} // namespace rdws::device_config
