#include "DeviceService.h"

namespace rdws::device {

std::vector<Device> DeviceService::findAll(const std::string& fieldId) {
    return repo_.findAll(fieldId);
}

std::optional<Device> DeviceService::findById(const std::string& id) {
    return repo_.findById(id);
}

std::string DeviceService::create(const DeviceCreate& data) {
    return repo_.create(data);
}

bool DeviceService::update(const std::string& id, const DeviceUpdate& data) {
    return repo_.update(id, data);
}

bool DeviceService::remove(const std::string& id) {
    return repo_.remove(id);
}

} // namespace rdws::device
