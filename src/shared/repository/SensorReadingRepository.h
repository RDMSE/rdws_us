#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::sensor_reading {

struct SensorReading {
  std::string id;
  std::string sensorId;
  std::string timestamp;
  std::string value;
  std::string createdAt;
};

class ISensorReadingRepository {
public:
  virtual ~ISensorReadingRepository() = default;

  // List readings for a sensor, optionally filtered by time range
  [[nodiscard]] virtual std::vector<SensorReading> findBySensorId(const std::string& sensorId,
                                                                  const std::string& from = {},
                                                                  const std::string& to = {}) = 0;

  [[nodiscard]] virtual std::optional<SensorReading> findById(const std::string& id) = 0;

  // Idempotent: relies on the UNIQUE(sensor_id, timestamp) constraint (V8 migration) —
  // a redelivered queue message (ReadingWriterService) silently no-ops instead of
  // duplicating the row. Returns true as long as the statement itself succeeds
  // (whether or not a row was actually inserted).
  [[nodiscard]] virtual bool insert(const std::string& sensorId, const std::string& timestamp,
                                    const std::string& value) = 0;
};

class SensorReadingRepository : public ISensorReadingRepository {
public:
  explicit SensorReadingRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<SensorReading> findBySensorId(const std::string& sensorId,
                                                          const std::string& from = {},
                                                          const std::string& to = {}) override;

  [[nodiscard]] std::optional<SensorReading> findById(const std::string& id) override;
  [[nodiscard]] bool insert(const std::string& sensorId, const std::string& timestamp,
                            const std::string& value) override;

private:
  rdws::database::IDatabase& db_;

  [[nodiscard]] static SensorReading readingFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::sensor_reading
