#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::sensor {

struct Sensor {
  std::string id;
  std::string deviceId;
  std::string type;
  std::string unit;
  std::string location; // WKT or empty
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct SensorCreate {
  std::string deviceId;
  std::string type;
  std::string unit;
  std::string updatedBy;
};

struct SensorUpdate {
  std::string type;
  std::string unit;
  std::string updatedBy;
};

class ISensorRepository {
public:
  virtual ~ISensorRepository() = default;

  [[nodiscard]] virtual std::vector<Sensor> findAll(const std::string& deviceId = {}) = 0;
  [[nodiscard]] virtual std::optional<Sensor> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::string create(const SensorCreate& data) = 0;
  [[nodiscard]] virtual bool update(const std::string& id, const SensorUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& id) = 0;
};

class SensorRepository : public ISensorRepository {
public:
  explicit SensorRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Sensor> findAll(const std::string& deviceId = {}) override;
  [[nodiscard]] std::optional<Sensor> findById(const std::string& id) override;
  [[nodiscard]] std::string create(const SensorCreate& data) override;
  [[nodiscard]] bool update(const std::string& id, const SensorUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& id) override;

private:
  rdws::database::IDatabase& db_;

  static Sensor sensorFromRow(rdws::database::IResultSet& rs);
};

} // namespace rdws::sensor
