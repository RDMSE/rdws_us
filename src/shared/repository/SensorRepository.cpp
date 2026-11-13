// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "SensorRepository.h"

namespace rdws::sensor {

static constexpr auto kSelectCols =
    "SELECT id, public_id, device_id, device_public_id, type, unit, "
    "ST_AsText(location) AS location, created_at, updated_at, updated_by FROM sensors";

Sensor SensorRepository::sensorFromRow(rdws::database::IResultSet& rs) {
  Sensor s;
  s.id = rs.getString("id");
  s.publicId = rs.getString("public_id");
  s.deviceId = rs.getString("device_id");
  s.devicePublicId = rs.getString("device_public_id");
  s.type = rs.getString("type");
  s.unit = rs.getString("unit");
  s.location = rs.isNull("location") ? "" : rs.getString("location");
  s.createdAt = rs.getString("created_at");
  s.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
  s.updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by");
  return s;
}

std::vector<Sensor> SensorRepository::findAll(const std::string& devicePublicId) {
  std::string query = std::string(kSelectCols);
  std::vector<std::string> params = {};
  if (devicePublicId.empty()) {
    query += " ORDER BY id";
  } else {
    query += " WHERE device_public_id = $1 ORDER BY id";
    params = {devicePublicId};
  }
  auto rs = db_.execQuery(query, params);
  std::vector<Sensor> sensors;
  while (rs->next()) {
    sensors.push_back(sensorFromRow(*rs));
  }
  return sensors;
}

std::optional<Sensor> SensorRepository::findOne(const std::string& column,
                                                const std::string& value) {
  auto rs = db_.execQuery(std::string(kSelectCols) + " WHERE " + column + " = $1", {value});
  if (!rs->next()) {
    return std::nullopt;
  }
  return sensorFromRow(*rs);
}

std::optional<Sensor> SensorRepository::findById(const std::string& id) {
  return findOne("id", id);
}

std::optional<Sensor> SensorRepository::findByPublicId(const std::string& publicId) {
  return findOne("public_id", publicId);
}

std::string SensorRepository::create(const SensorCreate& data) {
  auto rs = db_.execQuery("INSERT INTO sensors (device_id, device_public_id, type, unit, "
                          "updated_by) VALUES ($1, $2, $3, $4, $5) RETURNING public_id",
                          {data.deviceId, data.devicePublicId, data.type, data.unit,
                           data.updatedBy});
  if (!rs->next()) {
    return {};
  }
  return rs->getString("public_id");
}

bool SensorRepository::update(const std::string& publicId, const SensorUpdate& data) {
  // execCommand can't tell "no such sensor" from success; RETURNING can
  auto rs = db_.execQuery("UPDATE sensors SET type=$1, unit=$2, updated_at=now(), updated_by=$3 "
                          "WHERE public_id=$4 RETURNING id",
                          {data.type, data.unit, data.updatedBy, publicId});
  return rs->next();
}

bool SensorRepository::remove(const std::string& publicId) {
  auto rs = db_.execQuery("DELETE FROM sensors WHERE public_id = $1 RETURNING id", {publicId});
  return rs->next();
}

} // namespace rdws::sensor
