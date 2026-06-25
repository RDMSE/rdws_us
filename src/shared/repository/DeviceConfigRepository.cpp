#include "DeviceConfigRepository.h"

namespace rdws::device_config {

DeviceConfig DeviceConfigRepository::configFromRow(rdws::database::IResultSet& rs)
{
    DeviceConfig c;
    c.id        = rs.getString("id");
    c.deviceId  = rs.getString("device_id");
    c.config    = rs.getString("config");
    c.createdAt = rs.getString("created_at");
    c.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
    return c;
}

std::optional<DeviceConfig> DeviceConfigRepository::findByDeviceId(const std::string& deviceId)
{
    std::string query = "SELECT id, device_id, config::text AS config, created_at, updated_at, updated_by "
                        "FROM device_configurations WHERE device_id = $1 ORDER BY id DESC LIMIT 1";
    std::vector<std::string> params = {deviceId};
    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return std::nullopt;
    }
    return configFromRow(*rs);
}

std::string DeviceConfigRepository::create(const DeviceConfigCreate& data)
{
    std::string query = "INSERT INTO device_configurations (device_id, config) VALUES ($1, $2::jsonb) RETURNING id";
    std::vector<std::string> params = {data.deviceId, data.configJson};
    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return {};
    }
    return rs->getString("id");
}

bool DeviceConfigRepository::update(const std::string& deviceId, const DeviceConfigUpdate& data)
{
    std::string query = "UPDATE device_configurations SET config=$1::jsonb, updated_at=now() WHERE device_id=$2";
    std::vector<std::string> params = {data.configJson, deviceId};
    return db_.execCommand(query, params);
}

bool DeviceConfigRepository::remove(const std::string& deviceId)
{
    std::string query = "DELETE FROM device_configurations WHERE device_id = $1";
    std::vector<std::string> params = {deviceId};
    return db_.execCommand(query, params);
}

} // namespace rdws::device_config
