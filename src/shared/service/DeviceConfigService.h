// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/DeviceConfigRepository.h"
#include "../repository/SensorRepository.h"
#include "client/IDeclaredConfigSource.h"
#include "../types/service_result.h"

namespace rdws::device_config {

// Whether the current config version runs on the station (GET /devices/{id}/config/status).
struct ConfigStatus {
  int version = 0;             // device_configurations.version, the current one
  std::optional<int> declared; // what the station last said it runs
  std::optional<int> rejected; // cfg_rej of that same snapshot
  std::string declaredAt;      // empty when it never declared
  std::string state;           // configState()
};

// "applied" (declared == version), "rejected" (the latest snapshot refuses this very version),
// "pending" (not picked up yet) or "unknown" (never declared: simulator, old firmware).
// A cfg_rej of an older version doesn't make the current one rejected.
[[nodiscard]] std::string configState(int version, std::optional<int> declared,
                                      std::optional<int> rejected);

class DeviceConfigService {
public:
  // sensors: to check that the sensor_ids a weather station config points channels at
  // belong to the device and measure what the channel measures.
  // declaredSource: what the station declared (cfg_version/cfg_rej), owned by device_service.
  DeviceConfigService(IDeviceConfigRepository& repo, rdws::sensor::ISensorRepository& sensors,
                      IDeclaredConfigSource& declaredSource)
      : repo_(repo), sensors_(sensors), declaredSource_(declaredSource) {}

  // The API addresses the config by the device's public id (V21)
  [[nodiscard]] std::optional<DeviceConfig> findByDevicePublicId(const std::string& devicePublicId);
  // 404 when the device has no config, i.e. doesn't exist (the config is 1:1, V3); the
  // declared-config lookup's own failure (gateway down, timeout) passes through as is.
  [[nodiscard]] rdws::types::ServiceResult<ConfigStatus> status(const std::string& devicePublicId);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& devicePublicId,
                                                    const DeviceConfigUpdate& data);

private:
  IDeviceConfigRepository& repo_;
  rdws::sensor::ISensorRepository& sensors_;
  IDeclaredConfigSource& declaredSource_;

  [[nodiscard]] static std::string checkTriggers(const std::string& mergedJson);
  [[nodiscard]] std::string checkSensors(const std::string& deviceId,
                                         const std::string& mergedJson);
};

} // namespace rdws::device_config
