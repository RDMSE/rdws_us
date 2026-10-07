#pragma once
#include "../repository/DeviceConfigRepository.h"
#include "../repository/SensorRepository.h"
#include "../types/service_result.h"

namespace rdws::device_config {

class DeviceConfigService {
public:
  // sensors: to check that the sensor_ids a weather station config points channels at
  // belong to the device and measure what the channel measures.
  DeviceConfigService(IDeviceConfigRepository& repo, rdws::sensor::ISensorRepository& sensors)
      : repo_(repo), sensors_(sensors) {}

  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& deviceId,
                                                    const DeviceConfigUpdate& data);

private:
  IDeviceConfigRepository& repo_;
  rdws::sensor::ISensorRepository& sensors_;

  [[nodiscard]] std::string checkSensors(const std::string& deviceId,
                                         const std::string& mergedJson);
};

} // namespace rdws::device_config
