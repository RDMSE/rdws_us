#pragma once
#include "../repository/SensorRepository.h"

namespace rdws::sensor {

class SensorService {
public:
    explicit SensorService(ISensorRepository& repo) : repo_(repo) {}

    [[nodiscard]] std::vector<Sensor> findAll(const std::string& deviceId = {});
    [[nodiscard]] std::optional<Sensor> findById(const std::string& id);
    [[nodiscard]] std::string create(const SensorCreate& data);
    [[nodiscard]] bool update(const std::string& id, const SensorUpdate& data);
    [[nodiscard]] bool remove(const std::string& id);

private:
    ISensorRepository& repo_;
};

} // namespace rdws::sensor
