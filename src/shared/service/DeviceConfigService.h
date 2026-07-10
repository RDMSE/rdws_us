#pragma once
#include "../repository/DeviceConfigRepository.h"
#include "../types/service_result.h"

namespace rdws::device_config {

class DeviceConfigService {
public:
  explicit DeviceConfigService(IDeviceConfigRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& deviceId,
                                                    const DeviceConfigUpdate& data);

private:
  IDeviceConfigRepository& repo_;
};

} // namespace rdws::device_config
