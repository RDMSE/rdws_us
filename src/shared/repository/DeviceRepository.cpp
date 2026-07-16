#include "DeviceRepository.h"

namespace rdws::device {

static constexpr auto kSelectCols =
    "SELECT id, field_id, type, status, installation_date, "
    "ST_AsText(location) AS location, is_simulated, created_at, updated_at, updated_by "
    "FROM devices";

Device DeviceRepository::deviceFromRow(rdws::database::IResultSet& rs) {
  Device d;
  d.id = rs.getString("id");
  d.fieldId = rs.getString("field_id");
  d.type = rs.getString("type");
  d.status = rs.getString("status");
  d.installationDate = rs.isNull("installation_date") ? "" : rs.getString("installation_date");
  d.location = rs.isNull("location") ? "" : rs.getString("location");
  d.isSimulated = rs.getBool("is_simulated");
  d.createdAt = rs.getString("created_at");
  d.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
  d.updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by");
  return d;
}

std::vector<Device> DeviceRepository::findAll(const std::string& fieldId) {
  std::string query = std::string(kSelectCols);
  std::vector<std::string> params = {};

  std::unique_ptr<rdws::database::IResultSet> rs;
  if (fieldId.empty()) {
    query += " ORDER BY id";
  } else {
    query += " WHERE field_id = $1 ORDER BY id";
    params = {fieldId};
  }
  rs = db_.execQuery(query, params);
  std::vector<Device> devices;
  while (rs->next()) {
    devices.push_back(deviceFromRow(*rs));
  }
  return devices;
}

std::optional<Device> DeviceRepository::findById(const std::string& id) {
  std::string query = std::string(kSelectCols) + " WHERE id = $1";
  std::vector<std::string> params = {id};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return std::nullopt;
  }
  return deviceFromRow(*rs);
}

std::string DeviceRepository::create(const DeviceCreate& data) {
  const std::string deviceStatus = data.status.empty() ? "active" : data.status;
  const bool hasInstallationDate = !data.installationDate.empty();

  std::vector<std::string> columns = {"field_id", "type", "status", "is_simulated", "updated_by"};
  std::vector<std::string> params = {data.fieldId, data.type, deviceStatus,
                                     data.isSimulated ? "true" : "false", data.updatedBy};
  std::vector<std::string> placeholders = {"$1", "$2", "$3", "$4", "$5"};

  if (hasInstallationDate) {
    columns.emplace_back("installation_date");
    params.push_back(data.installationDate);
    placeholders.push_back("$" + std::to_string(params.size()));
  }

  std::string query = "INSERT INTO devices (";
  for (size_t i = 0; i < columns.size(); ++i) {
    query += (i > 0 ? ", " : "") + columns[i];
  }
  query += ") VALUES (";
  for (size_t i = 0; i < placeholders.size(); ++i) {
    query += (i > 0 ? ", " : "") + placeholders[i];
  }
  query += ") RETURNING id";

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return {};
  }
  return rs->getString("id");
}

bool DeviceRepository::update(const std::string& id, const DeviceUpdate& data) {
  std::string query =
      "UPDATE devices SET type=$1, status=$2, updated_at=now(), updated_by=$3 WHERE id=$4";
  std::vector<std::string> params = {data.type, data.status, data.updatedBy, id};
  return db_.execCommand(query, params);
}

bool DeviceRepository::remove(const std::string& id) {
  std::string query = "DELETE FROM devices WHERE id = $1";
  std::vector<std::string> params = {id};
  return db_.execCommand(query, params);
}

std::vector<SimulatedSensor>
DeviceRepository::findSimulatedSensorsByDeviceId(const std::string& deviceId) {
  const std::string query =
      "SELECT d.id AS device_id, s.id AS sensor_id, s.type AS sensor_type, s.unit "
      "FROM devices d JOIN sensors s ON s.device_id = d.id "
      "WHERE d.id = $1 AND d.is_simulated = true ORDER BY s.id";
  const std::vector<std::string> params = {deviceId};

  auto rs = db_.execQuery(query, params);
  std::vector<SimulatedSensor> sensors;
  while (rs->next()) {
    sensors.push_back(SimulatedSensor{
        .deviceId = rs->getString("device_id"),
        .sensorId = rs->getString("sensor_id"),
        .sensorType = rs->getString("sensor_type"),
        .unit = rs->getString("unit"),
    });
  }
  return sensors;
}

} // namespace rdws::device
