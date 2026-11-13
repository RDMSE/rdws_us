// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/SensorReadingRepository.h"
#include "../types/service_result.h"
#include "client/ISensorValidator.h"

namespace rdws::sensor_reading {

// The API addresses a sensor by its public id; sensor_readings is keyed by the internal one,
// which sensor_service resolves (sensor.ref). An unknown sensor is a 404, not an empty list.
class SensorReadingService {
public:
  SensorReadingService(ISensorReadingRepository& repo,
                       rdws::sensor::ISensorValidator& sensorValidator)
      : repo_(repo), sensorValidator_(sensorValidator) {}

  [[nodiscard]] rdws::types::ServiceResult<std::vector<SensorReading>>
  findBySensor(const std::string& sensorPublicId, const std::string& from = {},
               const std::string& to = {});
  // 404 when the sensor doesn't exist or the reading isn't one of its readings
  [[nodiscard]] rdws::types::ServiceResult<SensorReading>
  findInSensor(const std::string& sensorPublicId, const std::string& readingId);

private:
  ISensorReadingRepository& repo_;
  rdws::sensor::ISensorValidator& sensorValidator_;

  // The sensor's internal id, or the API error
  [[nodiscard]] rdws::sensor::SensorValidation resolveSensor(const std::string& sensorPublicId);
};

} // namespace rdws::sensor_reading
