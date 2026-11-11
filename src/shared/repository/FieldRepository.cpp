// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FieldRepository.h"

namespace rdws::field {

static constexpr auto kSelectCols =
    "SELECT id, public_id, farm_id, farm_public_id, name, area, ST_AsText(geometry) AS geometry, "
    "created_at, updated_at, updated_by FROM fields";

Field FieldRepository::fieldFromRow(rdws::database::IResultSet& rs) {
  return {
    .id = rs.getString("id"),
    .publicId = rs.getString("public_id"),
    .farmId = rs.getString("farm_id"),
    .farmPublicId = rs.getString("farm_public_id"),
    .name = rs.getString("name"),
    .area = rs.isNull("area") ? "" : rs.getString("area"),
    .geometry = rs.isNull("geometry") ? "" : rs.getString("geometry"),
    .createdAt = rs.getString("created_at"),
    .updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at"),
    .updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by")
  };
}

std::vector<Field> FieldRepository::findAll(const std::string& farmPublicId) {
  std::string query = std::string(kSelectCols);
  std::vector<std::string> params = {};
  if (farmPublicId.empty()) {
    query += " ORDER BY id";
  } else {
    query += " WHERE farm_public_id = $1 ORDER BY id";
    params = {farmPublicId};
  }

  auto rs = db_.execQuery(query, params);
  std::vector<Field> fields;
  while (rs->next()) {
    fields.push_back(fieldFromRow(*rs));
  }
  return fields;
}

std::optional<Field> FieldRepository::findOne(const std::string& column, const std::string& value) {
  auto rs = db_.execQuery(std::string(kSelectCols) + " WHERE " + column + " = $1", {value});
  if (!rs->next()) {
    return std::nullopt;
  }
  return fieldFromRow(*rs);
}

std::optional<Field> FieldRepository::findById(const std::string& id) {
  return findOne("id", id);
}

std::optional<Field> FieldRepository::findByPublicId(const std::string& publicId) {
  return findOne("public_id", publicId);
}

std::string FieldRepository::create(const FieldCreate& data) {
  std::string query = {};
  std::vector<std::string> params = {};

  if (data.area.empty()) {
    query = "INSERT INTO fields (farm_id, farm_public_id, name, updated_by) "
            "VALUES ($1, $2, $3, $4) RETURNING public_id";
    params = {data.farmId, data.farmPublicId, data.name, data.updatedBy};
  } else {
    query = "INSERT INTO fields (farm_id, farm_public_id, name, area, updated_by) "
            "VALUES ($1, $2, $3, $4, $5) RETURNING public_id";
    params = {data.farmId, data.farmPublicId, data.name, data.area, data.updatedBy};
  }

  const auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return {};
  }
  return rs->getString("public_id");
}

bool FieldRepository::update(const std::string& publicId, const FieldUpdate& data) {
  // execCommand can't tell "no such field" from success; RETURNING can
  auto rs = db_.execQuery(
      "UPDATE fields SET name=$1, updated_at=now(), updated_by=$2 WHERE public_id=$3 RETURNING id",
      {data.name, data.updatedBy, publicId});
  return rs->next();
}

bool FieldRepository::remove(const std::string& publicId) {
  auto rs = db_.execQuery("DELETE FROM fields WHERE public_id = $1 RETURNING id", {publicId});
  return rs->next();
}

} // namespace rdws::field
