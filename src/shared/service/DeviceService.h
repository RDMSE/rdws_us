#pragma once
#include "../repository/DeviceRepository.h"

namespace rdws::device {

class DeviceService {
public:
    explicit DeviceService(IDeviceRepository& repo) : repo_(repo) {}

    [[nodiscard]] std::vector<Device> findAll(const std::string& fieldId = {});
    [[nodiscard]] std::optional<Device> findById(const std::string& id);
    [[nodiscard]] std::string create(const DeviceCreate& data);
    [[nodiscard]] bool update(const std::string& id, const DeviceUpdate& data);
    [[nodiscard]] bool remove(const std::string& id);

private:
    IDeviceRepository& repo_;
};

} // namespace rdws::device
