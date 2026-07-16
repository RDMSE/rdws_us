#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::device {

struct Device {
  std::string id;
  std::string fieldId;
  std::string type;
  std::string status;
  std::string installationDate; // empty if NULL
  std::string location;         // WKT or empty
  bool isSimulated = false;
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct DeviceCreate {
  std::string fieldId;
  std::string type;
  std::string status;           // defaults to "active" if empty
  std::string installationDate; // empty = NULL
  bool isSimulated = false;     // immutable after creation (DB trigger enforced)
  std::string updatedBy;
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
};

class IDeviceRepository {
public:
  virtual ~IDeviceRepository() = default;

  [[nodiscard]] virtual std::vector<Device> findAll(const std::string& fieldId = {}) = 0;
  [[nodiscard]] virtual std::optional<Device> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::string create(const DeviceCreate& data) = 0;
  [[nodiscard]] virtual bool update(const std::string& id, const DeviceUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& id) = 0;
  // Sensors of a single simulated device (is_simulated = true), for SensorSimulatorService.
  [[nodiscard]] virtual std::vector<SimulatedSensor>
  findSimulatedSensorsByDeviceId(const std::string& deviceId) = 0;
};

class DeviceRepository : public IDeviceRepository {
public:
  explicit DeviceRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Device> findAll(const std::string& fieldId = {}) override;
  [[nodiscard]] std::optional<Device> findById(const std::string& id) override;
  [[nodiscard]] std::string create(const DeviceCreate& data) override;
  [[nodiscard]] bool update(const std::string& id, const DeviceUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& id) override;
  [[nodiscard]] std::vector<SimulatedSensor>
  findSimulatedSensorsByDeviceId(const std::string& deviceId) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static Device deviceFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::device
