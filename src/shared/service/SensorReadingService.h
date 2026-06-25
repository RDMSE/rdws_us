#pragma once
#include "../repository/SensorReadingRepository.h"

namespace rdws::sensor_reading {

class SensorReadingService {
public:
    explicit SensorReadingService(ISensorReadingRepository& repo) : repo_(repo) {}

    [[nodiscard]] std::vector<SensorReading>   findBySensorId(const std::string& sensorId,
                                                              const std::string& from = {},
                                                              const std::string& to   = {});
    [[nodiscard]] std::optional<SensorReading> findById(const std::string& id);

private:
    ISensorReadingRepository& repo_;
};

} // namespace rdws::sensor_reading
