#include "SensorReadingService.h"

namespace rdws::sensor_reading {

std::vector<SensorReading> SensorReadingService::findBySensorId(const std::string& sensorId,
                                                                const std::string& from,
                                                                const std::string& to) {
  return repo_.findBySensorId(sensorId, from, to);
}

std::optional<SensorReading> SensorReadingService::findById(const std::string& id) {
  return repo_.findById(id);
}

} // namespace rdws::sensor_reading
