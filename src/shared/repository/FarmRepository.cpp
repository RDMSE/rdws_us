// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FarmRepository.h"

namespace rdws::farm {

static constexpr auto kSelectCols = "SELECT id, public_id, name, ST_AsText(location) AS location, timezone, "
                                    "created_at, updated_at, updated_by FROM farms";

Farm FarmRepository::farmFromRow(rdws::database::IResultSet& rs) {
  Farm f;
  f.id = rs.getString("id");
  f.publicId = rs.getString("public_id");
  f.name = rs.getString("name");
  f.location = rs.isNull("location") ? "" : rs.getString("location");
  f.timezone = rs.getString("timezone");
  f.createdAt = rs.getString("created_at");
  f.updatedAt = rs.isNull("updated_at") ? "" : rs.getString("updated_at");
  f.updatedBy = rs.isNull("updated_by") ? "" : rs.getString("updated_by");
  return f;
}

std::vector<Farm> FarmRepository::findAll() {
  std::string query = std::string(kSelectCols) + " ORDER BY id";
  std::vector<std::string> params = {};

  auto rs = db_.execQuery(query, params);
  std::vector<Farm> farms;
  while (rs->next()) {
    farms.push_back(farmFromRow(*rs));
  }
  return farms;
}

std::optional<Farm> FarmRepository::findOne(const std::string& column, const std::string& value) {
  auto rs = db_.execQuery(std::string(kSelectCols) + " WHERE " + column + " = $1", {value});
  if (!rs->next()) {
    return std::nullopt;
  }
  return farmFromRow(*rs);
}

std::optional<Farm> FarmRepository::findById(const std::string& id) {
  return findOne("id", id);
}

std::optional<Farm> FarmRepository::findByPublicId(const std::string& publicId) {
  return findOne("public_id", publicId);
}

std::string FarmRepository::create(const FarmCreate& data) {
  // Optional columns are only listed when given, so the table's defaults apply otherwise
  std::vector<std::string> columns = {"name", "updated_by"};
  std::vector<std::string> values = {"$1", "$2"};
  std::vector<std::string> params = {data.name, data.updatedBy};
  if (!data.locationWkt.empty()) {
    params.push_back(data.locationWkt);
    columns.emplace_back("location");
    values.push_back("ST_SetSRID(ST_GeomFromText($" + std::to_string(params.size()) + "),4326)");
  }
  if (!data.timezone.empty()) {
    params.push_back(data.timezone);
    columns.emplace_back("timezone");
    values.push_back("$" + std::to_string(params.size()));
  }

  std::string sql = "INSERT INTO farms (";
  for (size_t i = 0; i < columns.size(); ++i) {
    sql += (i > 0 ? ", " : "") + columns[i];
  }
  sql += ") VALUES (";
  for (size_t i = 0; i < values.size(); ++i) {
    sql += (i > 0 ? ", " : "") + values[i];
  }
  sql += ") RETURNING public_id";

  auto rs = db_.execQuery(sql, params);
  if (!rs->next()) {
    return {};
  }
  return rs->getString("public_id");
}

bool FarmRepository::update(const std::string& publicId, const FarmUpdate& data) {
  // name is always replaced; location and timezone only when given
  std::vector<std::string> params = {data.name, data.updatedBy};
  std::string sql = "UPDATE farms SET name=$1, updated_by=$2, updated_at=now()";
  if (!data.locationWkt.empty()) {
    params.push_back(data.locationWkt);
    sql += ", location=ST_SetSRID(ST_GeomFromText($" + std::to_string(params.size()) + "),4326)";
  }
  if (!data.timezone.empty()) {
    params.push_back(data.timezone);
    sql += ", timezone=$" + std::to_string(params.size());
  }
  params.push_back(publicId);
  sql += " WHERE public_id=$" + std::to_string(params.size()) + " RETURNING id";

  // execCommand can't tell "no such farm" from success; RETURNING can
  auto rs = db_.execQuery(sql, params);
  return rs->next();
}

bool FarmRepository::isKnownTimezone(const std::string& name) {
  auto rs = db_.execQuery("SELECT 1 FROM pg_timezone_names WHERE name = $1", {name});
  return rs->next();
}

bool FarmRepository::remove(const std::string& publicId) {
  auto rs = db_.execQuery("DELETE FROM farms WHERE public_id = $1 RETURNING id", {publicId});
  return rs->next();
}

} // namespace rdws::farm
