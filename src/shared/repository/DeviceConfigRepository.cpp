// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceConfigRepository.h"

namespace rdws::device_config {

DeviceConfig DeviceConfigRepository::configFromRow(rdws::database::IResultSet& rs) {
  DeviceConfig c;
  c.id = rs.getString("id");
  c.deviceId = rs.getString("device_id");
  c.devicePublicId = rs.getString("device_public_id");
  c.config = rs.getString("config");
  c.version = rs.getInt("version");
  c.deviceType = rs.getString("device_type");
  c.isSimulated = rs.getBool("is_simulated");
  c.createdAt = rs.getString("created_at");
  c.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
  c.updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by");
  return c;
}

std::optional<DeviceConfig> DeviceConfigRepository::findOne(const std::string& column,
                                                        const std::string& value) {
  std::string query =
      "SELECT dc.id, dc.device_id, dc.device_public_id, dc.config::text AS config, dc.version, "
      "dc.created_at, dc.updated_at, dc.updated_by, d.type::text AS device_type, d.is_simulated "
      "FROM device_configurations dc JOIN devices d ON d.id = dc.device_id "
      "WHERE dc." + column + " = $1";
  auto rs = db_.execQuery(query, {value});
  if (!rs->next()) {
    return std::nullopt;
  }
  return configFromRow(*rs);
}

std::optional<DeviceConfig>
DeviceConfigRepository::findByDevicePublicId(const std::string& devicePublicId) {
  return findOne("device_public_id", devicePublicId);
}

std::optional<DeviceConfig> DeviceConfigRepository::findByDeviceId(const std::string& deviceId) {
  return findOne("device_id", deviceId);
}

bool DeviceConfigRepository::update(const std::string& deviceId, const DeviceConfigUpdate& data) {
  std::string query = "UPDATE device_configurations SET config=$1::jsonb, updated_at=now(), "
                       "updated_by=$2 WHERE device_id=$3";
  std::vector<std::string> params = {data.configJson, data.updatedBy, deviceId};
  return db_.execCommand(query, params);
}

} // namespace rdws::device_config
