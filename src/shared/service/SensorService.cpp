#include "SensorService.h"

namespace rdws::sensor {

std::vector<Sensor> SensorService::findAll(const std::string& deviceId) {
    return repo_.findAll(deviceId);
}

std::optional<Sensor> SensorService::findById(const std::string& id) {
    return repo_.findById(id);
}

std::string SensorService::create(const SensorCreate& data) {
    return repo_.create(data);
}

bool SensorService::update(const std::string& id, const SensorUpdate& data) {
    return repo_.update(id, data);
}

bool SensorService::remove(const std::string& id) {
    return repo_.remove(id);
}

} // namespace rdws::sensor
