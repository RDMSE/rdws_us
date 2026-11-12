// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::device {

struct Device {
  std::string id;            // internal (BIGSERIAL): FK target, never sent to API clients
  std::string publicId;      // UUID: the id the API exposes and receives
  std::string fieldId;       // internal
  std::string fieldPublicId; // the field's public id, copied on create (V20)
  std::string type;
  std::string status;
  std::string installationDate; // empty if NULL
  std::string location;         // WKT or empty
  std::optional<double> elevationM;           // V17, D7 of Plano_Indices_Derivados.md
  std::optional<std::string> elevationSource; // "dem", "gps" or "manual"; set with elevationM
  bool isSimulated = false;
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct DeviceCreate {
  std::string fieldId;       // internal, resolved from fieldPublicId by the service
  std::string fieldPublicId;
  std::string type;
  std::string status;           // defaults to "active" if empty
  std::string installationDate; // empty = NULL
  bool isSimulated = false;     // immutable after creation (DB trigger enforced)
  std::string updatedBy;
};

// Both ids of a new device: the credential is provisioned against the internal one, the API
// answers with the public one.
struct DeviceIds {
  std::string id;
  std::string publicId;
};

// One row per sensor, joined with its parent device — used by SensorSimulatorService
// to load a simulated device's full sensor list in one query.
struct SimulatedSensor {
  std::string deviceId;
  std::string sensorId;
  std::string sensorType;
  std::string unit;
};

struct DeviceUpdate {
  std::string type;
  std::string status;
  std::string updatedBy;
  // Absent = keep; an empty inner optional = clear; a value = set with source "manual" (the
  // highest priority in D7, so a later automatic source never overwrites it).
  std::optional<std::optional<double>> elevationM;
};

class IDeviceRepository {
public:
  virtual ~IDeviceRepository() = default;

  // fieldPublicId empty = every device
  [[nodiscard]] virtual std::vector<Device> findAll(const std::string& fieldPublicId = {}) = 0;
  // By the internal id: for other services (internal capabilities), never from an API path
  [[nodiscard]] virtual std::optional<Device> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::optional<Device> findByPublicId(const std::string& publicId) = 0;
  // Empty ids on failure
  [[nodiscard]] virtual DeviceIds create(const DeviceCreate& data) = 0;
  // false = no device with that public id
  [[nodiscard]] virtual bool update(const std::string& publicId, const DeviceUpdate& data) = 0;
  // Narrow, single-field update (location only) - lets a low-privilege caller like
  // IngestionService report device position without needing type/status (device.update
  // requires both). Mirrors the device_credential.* internal-capability pattern.
  [[nodiscard]] virtual bool updateLocation(const std::string& id,
                                            const std::string& locationWkt) = 0;
  // false = no device with that public id
  [[nodiscard]] virtual bool remove(const std::string& publicId) = 0;
  // Sensors of a single simulated device (is_simulated = true), for SensorSimulatorService.
  [[nodiscard]] virtual std::vector<SimulatedSensor>
  findSimulatedSensorsByDeviceId(const std::string& deviceId) = 0;
};

class DeviceRepository : public IDeviceRepository {
public:
  explicit DeviceRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Device> findAll(const std::string& fieldPublicId = {}) override;
  [[nodiscard]] std::optional<Device> findById(const std::string& id) override;
  [[nodiscard]] std::optional<Device> findByPublicId(const std::string& publicId) override;
  [[nodiscard]] DeviceIds create(const DeviceCreate& data) override;
  [[nodiscard]] bool update(const std::string& publicId, const DeviceUpdate& data) override;
  [[nodiscard]] bool updateLocation(const std::string& id, const std::string& locationWkt) override;
  [[nodiscard]] bool remove(const std::string& publicId) override;
  [[nodiscard]] std::vector<SimulatedSensor>
  findSimulatedSensorsByDeviceId(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Device deviceFromRow(rdws::database::IResultSet& rs);
  [[nodiscard]] std::optional<Device> findOne(const std::string& column, const std::string& value);
};

} // namespace rdws::device
