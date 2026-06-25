#include "DeviceRepository.h"

namespace rdws::device {

static constexpr auto kSelectCols =
    "SELECT id, field_id, type, status, installation_date, "
    "ST_AsText(location) AS location, created_at, updated_at, updated_by FROM devices";

Device DeviceRepository::deviceFromRow(rdws::database::IResultSet& rs)
{
    Device d;
    d.id               = rs.getString("id");
    d.fieldId          = rs.getString("field_id");
    d.type             = rs.getString("type");
    d.status           = rs.getString("status");
    d.installationDate = rs.isNull("installation_date") ? "" : rs.getString("installation_date");
    d.location         = rs.isNull("location")          ? "" : rs.getString("location");
    d.createdAt        = rs.getString("created_at");
    d.updatedAt        = rs.isNull("updated_at")        ? "" : rs.getString("updated_at");
    d.updatedBy        = rs.isNull("updated_by")        ? "" : rs.getString("updated_by");
    return d;
}

std::vector<Device> DeviceRepository::findAll(const std::string& fieldId)
{
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

std::optional<Device> DeviceRepository::findById(const std::string& id)
{
    std::string query = std::string(kSelectCols) + " WHERE id = $1";
    std::vector<std::string> params = {id};

    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return std::nullopt;
    }
    return deviceFromRow(*rs);
}

std::string DeviceRepository::create(const DeviceCreate& data)
{
    const std::string deviceStatus = data.status.empty() ? "active" : data.status;
    std::string query = "INSERT INTO devices (field_id, type, status) VALUES ($1, $2, $3) RETURNING id";
    std::vector<std::string> params = {data.fieldId, data.type, deviceStatus};
    auto rs = db_.execQuery(query, params);
    if (!rs->next()) {
        return {};
    }
    return rs->getString("id");
}

bool DeviceRepository::update(const std::string& id, const DeviceUpdate& data)
{
    std::string query = "UPDATE devices SET type=$1, status=$2, updated_at=now() WHERE id=$3";
    std::vector<std::string> params = {data.type, data.status, id};
    return db_.execCommand(query, params);
}

bool DeviceRepository::remove(const std::string& id)
{
    std::string query = "DELETE FROM devices WHERE id = $1";
    std::vector<std::string> params = {id};
    return db_.execCommand(query, params);
}

} // namespace rdws::device
