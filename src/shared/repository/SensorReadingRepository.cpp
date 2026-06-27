#include "SensorReadingRepository.h"

namespace rdws::sensor_reading {

SensorReading SensorReadingRepository::readingFromRow(rdws::database::IResultSet& rs) {
  SensorReading r;
  r.id = rs.getString("id");
  r.sensorId = rs.getString("sensor_id");
  r.timestamp = rs.getString("timestamp");
  r.value = rs.getString("value");
  r.createdAt = rs.getString("created_at");
  return r;
}

std::vector<SensorReading> SensorReadingRepository::findBySensorId(const std::string& sensorId,
                                                                   const std::string& from,
                                                                   const std::string& to) {
  std::string query = {};
  std::vector<std::string> params = {};

  if (!from.empty() && !to.empty()) {
    query = "SELECT id, sensor_id, timestamp, value::text AS value, created_at "
            "FROM sensor_readings WHERE sensor_id = $1 AND timestamp BETWEEN $2 AND $3 "
            "ORDER BY timestamp DESC";
    params = {sensorId, from, to};
  } else if (!from.empty()) {
    query = "SELECT id, sensor_id, timestamp, value::text AS value, created_at "
            "FROM sensor_readings WHERE sensor_id = $1 AND timestamp >= $2 "
            "ORDER BY timestamp DESC";
    params = {sensorId, from};
  } else {
    query = "SELECT id, sensor_id, timestamp, value::text AS value, created_at "
            "FROM sensor_readings WHERE sensor_id = $1 "
            "ORDER BY timestamp DESC LIMIT 1000";
    params = {sensorId};
  }

  const auto rs = db_.execQuery(query, params);

  std::vector<SensorReading> readings;
  while (rs->next()) {
    readings.push_back(readingFromRow(*rs));
  }
  return readings;
}

std::optional<SensorReading> SensorReadingRepository::findById(const std::string& id) {
  std::string query = "SELECT id, sensor_id, timestamp, value::text AS value, created_at "
                      "FROM sensor_readings WHERE id = $1";
  std::vector<std::string> params = {id};

  auto rs = db_.execQuery(query, params);
  if (!rs->next()) {
    return std::nullopt;
  }
  return readingFromRow(*rs);
}

} // namespace rdws::sensor_reading
