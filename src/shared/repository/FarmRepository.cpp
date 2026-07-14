#include "FarmRepository.h"

namespace rdws::farm {

static constexpr auto kSelectCols = "SELECT id, name, ST_AsText(location) AS location, "
                                    "created_at, updated_at, updated_by FROM farms";

Farm FarmRepository::farmFromRow(rdws::database::IResultSet& rs) {
  Farm f;
  f.id = rs.getString("id");
  f.name = rs.getString("name");
  f.location = rs.isNull("location") ? "" : rs.getString("location");
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

std::optional<Farm> FarmRepository::findById(const std::string& id) {
  std::string query = std::string(kSelectCols) + " WHERE id = $1";
  std::vector<std::string> params = {id};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return std::nullopt;
  }
  return farmFromRow(*rs);
}

std::string FarmRepository::create(const FarmCreate& data) {
  const bool hasLocation = !data.locationWkt.empty();
  const std::string sql =
      hasLocation ? "INSERT INTO farms (name, location, updated_by) VALUES ($1, "
                    "ST_SetSRID(ST_GeomFromText($2),4326), $3) RETURNING id"
                  : "INSERT INTO farms (name, updated_by) VALUES ($1, $2) RETURNING id";

  const std::vector<std::string> params =
      hasLocation ? std::vector<std::string>{data.name, data.locationWkt, data.updatedBy}
                  : std::vector<std::string>{data.name, data.updatedBy};

  auto rs = db_.execQuery(sql, params);
  if (!rs->next()) {
    return {};
  }
  return rs->getString("id");
}

bool FarmRepository::update(const std::string& id, const FarmUpdate& data) {
  const bool hasLocation = !data.locationWkt.empty();
  const std::string sql =
      hasLocation ? "UPDATE farms SET name=$1, location=ST_SetSRID(ST_GeomFromText($2),4326), "
                    "updated_at=now(), updated_by=$3 WHERE id=$4"
                  : "UPDATE farms SET name=$1, updated_at=now(), updated_by=$2 WHERE id=$3";

  const std::vector<std::string> params =
      hasLocation ? std::vector<std::string>{data.name, data.locationWkt, data.updatedBy, id}
                  : std::vector<std::string>{data.name, data.updatedBy, id};

  return db_.execCommand(sql, params);
}

bool FarmRepository::remove(const std::string& id) {
  std::string query = "DELETE FROM farms WHERE id = $1";
  std::vector<std::string> params = {id};

  return db_.execCommand(query, params);
}

} // namespace rdws::farm
