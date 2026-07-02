#include "FieldRepository.h"

namespace rdws::field {

static constexpr auto kSelectCols =
    "SELECT id, farm_id, name, area, ST_AsText(geometry) AS geometry, "
    "created_at, updated_at, updated_by FROM fields";

Field FieldRepository::fieldFromRow(rdws::database::IResultSet& rs) {
  return {
    .id = rs.getString("id"),
    .farmId = rs.getString("farm_id"),
    .name = rs.getString("name"),
    .area = rs.isNull("area") ? "" : rs.getString("area"),
    .geometry = rs.isNull("geometry") ? "" : rs.getString("geometry"),
    .createdAt = rs.getString("created_at"),
    .updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at"),
    .updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by")
  };
}

std::vector<Field> FieldRepository::findAll(const std::string& farmId) {
  std::string query = std::string(kSelectCols);
  std::vector<std::string> params = {};

  std::unique_ptr<rdws::database::IResultSet> rs;
  if (farmId.empty()) {
    query += " ORDER BY id";
  } else {
    query += " WHERE farm_id = $1 ORDER BY id";
    params = {farmId};
  }

  rs = db_.execQuery(query, params);

  std::vector<Field> fields;
  while (rs->next()) {
    fields.push_back(fieldFromRow(*rs));
  }
  return fields;
}

std::optional<Field> FieldRepository::findById(const std::string& id) {
  std::string query = std::string(kSelectCols) + " WHERE id = $1";
  std::vector<std::string> params = {id};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return std::nullopt;
  }
  return fieldFromRow(*rs);
}

std::string FieldRepository::create(const FieldCreate& data) {
  std::string query = {};
  std::vector<std::string> params = {};

  if (data.area.empty()) {
    query = "INSERT INTO fields (farm_id, name) VALUES ($1, $2) RETURNING id";
    params = {data.farmId, data.name};
  } else {
    query = "INSERT INTO fields (farm_id, name, area) VALUES ($1, $2, $3) RETURNING id";
    params = {data.farmId, data.name, data.area};
  }

  const auto rs = db_.execQuery(query, params);

  if (!rs->next()) {
    return {};
  }
  return rs->getString("id");
}

bool FieldRepository::update(const std::string& id, const FieldUpdate& data) {
  std::string query = "UPDATE fields SET name=$1, updated_at=now() WHERE id=$2";
  std::vector<std::string> params = {data.name, id};

  return db_.execCommand(query, params);
}

bool FieldRepository::remove(const std::string& id) {
  std::string query = "DELETE FROM fields WHERE id = $1";
  std::vector<std::string> params = {id};
  return db_.execCommand(query, params);
}

} // namespace rdws::field
