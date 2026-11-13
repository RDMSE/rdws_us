// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "SensorReadingService.h"

namespace rdws::sensor_reading {

using rdws::types::ServiceResult;

rdws::sensor::SensorValidation
SensorReadingService::resolveSensor(const std::string& sensorPublicId) {
  auto validation = sensorValidator_.resolve(sensorPublicId);
  if (!validation.found) {
    validation.errorMessage = validation.statusCode == 404
                                  ? "Sensor not found for id " + sensorPublicId
                                  : "Sensor validation failed for id " + sensorPublicId + ": " +
                                        validation.errorMessage;
  }
  return validation;
}

ServiceResult<std::vector<SensorReading>>
SensorReadingService::findBySensor(const std::string& sensorPublicId, const std::string& from,
                                   const std::string& to) {
  const auto sensor = resolveSensor(sensorPublicId);
  if (!sensor.found) {
    return ServiceResult<std::vector<SensorReading>>::error(sensor.errorMessage,
                                                            sensor.statusCode);
  }
  return ServiceResult<std::vector<SensorReading>>::success(
      repo_.findBySensorId(sensor.id, from, to));
}

ServiceResult<SensorReading> SensorReadingService::findInSensor(const std::string& sensorPublicId,
                                                                const std::string& readingId) {
  const auto sensor = resolveSensor(sensorPublicId);
  if (!sensor.found) {
    return ServiceResult<SensorReading>::error(sensor.errorMessage, sensor.statusCode);
  }
  auto reading = repo_.findInSensor(sensor.id, readingId);
  if (!reading) {
    return ServiceResult<SensorReading>::error("Reading not found", 404);
  }
  return ServiceResult<SensorReading>::success(std::move(*reading));
}

} // namespace rdws::sensor_reading
