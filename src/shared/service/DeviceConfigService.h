#pragma once
#include "../repository/DeviceConfigRepository.h"

namespace rdws::device_config {

class DeviceConfigService {
public:
  explicit DeviceConfigService(IDeviceConfigRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId);
  [[nodiscard]] std::string create(const DeviceConfigCreate& data);
  [[nodiscard]] bool update(const std::string& deviceId, const DeviceConfigUpdate& data);
  [[nodiscard]] bool remove(const std::string& deviceId);

private:
  IDeviceConfigRepository& repo_;
};

} // namespace rdws::device_config
