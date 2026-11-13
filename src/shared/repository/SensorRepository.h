// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../database/idatabase.h"

#include <optional>
#include <string>
#include <vector>

namespace rdws::sensor {

struct Sensor {
  std::string id;             // internal: sensor_readings, SenML names and the config use it
  std::string publicId;       // UUID: the id the API exposes and receives
  std::string deviceId;       // internal
  std::string devicePublicId; // the device's public id, copied on create (V22)
  std::string type;
  std::string unit;
  std::string location; // WKT or empty
  std::string createdAt;
  std::string updatedAt;
  std::string updatedBy;
};

struct SensorCreate {
  std::string deviceId;       // internal, resolved from devicePublicId by the service
  std::string devicePublicId;
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

  // devicePublicId empty = every sensor
  [[nodiscard]] virtual std::vector<Sensor> findAll(const std::string& devicePublicId = {}) = 0;
  // By the internal id: for internal capabilities, never from an API path
  [[nodiscard]] virtual std::optional<Sensor> findById(const std::string& id) = 0;
  [[nodiscard]] virtual std::optional<Sensor> findByPublicId(const std::string& publicId) = 0;
  // Returns the new sensor's public id
  [[nodiscard]] virtual std::string create(const SensorCreate& data) = 0;
  // false = no sensor with that public id
  [[nodiscard]] virtual bool update(const std::string& publicId, const SensorUpdate& data) = 0;
  [[nodiscard]] virtual bool remove(const std::string& publicId) = 0;
};

class SensorRepository : public ISensorRepository {
public:
  explicit SensorRepository(rdws::database::IDatabase& db) : db_(db) {}

  [[nodiscard]] std::vector<Sensor> findAll(const std::string& devicePublicId = {}) override;
  [[nodiscard]] std::optional<Sensor> findById(const std::string& id) override;
  [[nodiscard]] std::optional<Sensor> findByPublicId(const std::string& publicId) override;
  [[nodiscard]] std::string create(const SensorCreate& data) override;
  [[nodiscard]] bool update(const std::string& publicId, const SensorUpdate& data) override;
  [[nodiscard]] bool remove(const std::string& publicId) override;

private:
  rdws::database::IDatabase& db_;

  static Sensor sensorFromRow(rdws::database::IResultSet& rs);
  [[nodiscard]] std::optional<Sensor> findOne(const std::string& column, const std::string& value);
};

} // namespace rdws::sensor
