// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>

namespace rdws::device_config {

struct DeviceConfig {
  std::string id;
  std::string deviceId;       // internal: sensors, history and telemetry are keyed by it
  std::string devicePublicId; // the device's public id, copied by the V3 trigger (V21)
  std::string config; // JSON string (from JSONB)
  int version = 0;    // owned by the DB, bumped on every config change (V12)
  std::string deviceType; // devices.type, picks the config schema to validate against
  bool isSimulated = false;
  std::string createdAt;
  std::string updatedAt; // empty if NULL
  std::string updatedBy;
};

struct DeviceConfigUpdate {
  std::string configJson; // serialized JSON
  std::string updatedBy;
};

// device_config is 1:1 with device — the row is created by a trigger in Postgres alongside the
// device (db/migrations/V3__device_config_one_to_one.sql), never by the application. Therefore,
// the repository does not have create()/remove(): it always exists, only findByDeviceId/update.
class IDeviceConfigRepository {
public:
  virtual ~IDeviceConfigRepository() = default;

  // By the device's public id, what the API path carries
  [[nodiscard]] virtual std::optional<DeviceConfig>
  findByDevicePublicId(const std::string& devicePublicId) = 0;
  // By the internal device id: SensorSimulatorService, which reads the table directly
  [[nodiscard]] virtual std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) = 0;
  // By the internal device id, taken from the row found above
  [[nodiscard]] virtual bool update(const std::string& deviceId,
                                    const DeviceConfigUpdate& data) = 0;
};

class DeviceConfigRepository : public IDeviceConfigRepository {
public:
  explicit DeviceConfigRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::optional<DeviceConfig>
  findByDevicePublicId(const std::string& devicePublicId) override;
  [[nodiscard]] std::optional<DeviceConfig> findByDeviceId(const std::string& deviceId) override;
  [[nodiscard]] bool update(const std::string& deviceId, const DeviceConfigUpdate& data) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static DeviceConfig configFromRow(rdws::database::IResultSet& rs);
  [[nodiscard]] std::optional<DeviceConfig> findOne(const std::string& column,
                                                    const std::string& value);
};

} // namespace rdws::device_config
