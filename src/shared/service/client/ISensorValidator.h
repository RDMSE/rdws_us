// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace rdws::sensor {

// Result of resolving a sensor's public id. When found, id is the sensor's internal id (what
// sensor_readings is keyed by). When found is false, statusCode carries the cause so callers can
// tell "sensor not found" (404) from transient failures (gateway disconnected, invoke
// timeout/503) instead of collapsing both into a 404.
struct SensorValidation {
  bool found = false;
  std::string id; // internal, set when found
  int statusCode = 404;
  std::string errorMessage;
};

// Narrow interface used by domain services that only need to check a sensor id they received
// (a public id) and get the internal id their rows are keyed by, without pulling in the rest of
// ISensorRepository's CRUD surface.
class ISensorValidator {
public:
  virtual ~ISensorValidator() = default;

  [[nodiscard]] virtual SensorValidation resolve(const std::string& sensorPublicId) = 0;
};

} // namespace rdws::sensor
