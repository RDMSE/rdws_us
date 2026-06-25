#include "SensorRepository.h"

namespace rdws::sensor {

static constexpr auto kSelectCols =
    "SELECT id, device_id, type, unit, ST_AsText(location) AS location, "
    "created_at, updated_at, updated_by FROM sensors";

Sensor SensorRepository::sensorFromRow(rdws::database::IResultSet& rs)
{
    Sensor s;
    s.id        = rs.getString("id");
    s.deviceId  = rs.getString("device_id");
    s.type      = rs.getString("type");
    s.unit      = rs.getString("unit");
    s.location  = rs.isNull("location")   ? "" : rs.getString("location");
    s.createdAt = rs.getString("created_at");
    s.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
    s.updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by");
    return s;
}

std::vector<Sensor> SensorRepository::findAll(const std::string& deviceId)
{
    std::string query = std::string(kSelectCols);
    std::vector<std::string> params = {};

    if (deviceId.empty()) {
        query += " ORDER BY id";
    } else {
        query += " WHERE device_id = $1 ORDER BY id";
        params = {deviceId};
    }
    auto rs = db_.execQuery(query, params);
    std::vector<Sensor> sensors;
    while (rs->next()) {
        sensors.push_back(sensorFromRow(*rs));
    }
    return sensors;
}

std::optional<Sensor> SensorRepository::findById(const std::string& id)
{
    std::string query = std::string(kSelectCols) + " WHERE id = $1";
    std::vector<std::string> params = {id};

    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return std::nullopt;
    }
    return sensorFromRow(*rs);
}

std::string SensorRepository::create(const SensorCreate& data)
{
    std::string query = "INSERT INTO sensors (device_id, type, unit) VALUES ($1, $2, $3) RETURNING id";
    std::vector<std::string> params = {data.deviceId, data.type, data.unit};

    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return {};
    }
    return rs->getString("id");
}

bool SensorRepository::update(const std::string& id, const SensorUpdate& data)
{
    std::string query = "UPDATE sensors SET type=$1, unit=$2, updated_at=now() WHERE id=$3";
    std::vector<std::string> params = {data.type, data.unit, id};
    return db_.execCommand(query, params);
}

bool SensorRepository::remove(const std::string& id)
{
    std::string query = "DELETE FROM sensors WHERE id = $1";
    std::vector<std::string> params = {id};
    return db_.execCommand(query, params);
}

} // namespace rdws::sensor
