// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "SensorService.h"

namespace rdws::sensor {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

namespace {

std::string deviceError(const rdws::device::DeviceValidation& v, const std::string& publicId) {
  return v.statusCode == 404
             ? "Device not found for id " + publicId
             : "Device validation failed for id " + publicId + ": " + v.errorMessage;
}

} // namespace

ServiceResult<std::vector<Sensor>> SensorService::findAll(const std::string& devicePublicId) {
  if (!devicePublicId.empty()) {
    if (const auto device = deviceValidator_.resolve(devicePublicId); !device.found) {
      return ServiceResult<std::vector<Sensor>>::error(deviceError(device, devicePublicId),
                                                       device.statusCode);
    }
  }
  return ServiceResult<std::vector<Sensor>>::success(repo_.findAll(devicePublicId));
}

std::optional<Sensor> SensorService::findById(const std::string& id) {
  return repo_.findById(id);
}

std::optional<Sensor> SensorService::findByPublicId(const std::string& publicId) {
  return repo_.findByPublicId(publicId);
}

ServiceResult<std::string> SensorService::create(SensorCreate data) {
  if (data.devicePublicId.empty()) {
    return ServiceResult<std::string>::error("Missing field: device_id", 400);
  }
  const auto device = deviceValidator_.resolve(data.devicePublicId);
  if (!device.found) {
    return ServiceResult<std::string>::error(deviceError(device, data.devicePublicId),
                                             device.statusCode);
  }
  data.deviceId = device.id;
  const std::string id = repo_.create(data);
  if (id.empty()) {
    return ServiceResult<std::string>::error("Failed to create sensor", 500);
  }
  return ServiceResult<std::string>::success(id, 201);
}

OperationResult SensorService::update(const std::string& publicId, const SensorUpdate& data) {
  if (!repo_.update(publicId, data)) {
    return OperationResult::error("Sensor not found", 404);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

bool SensorService::remove(const std::string& publicId) {
  return repo_.remove(publicId);
}

std::vector<Sensor> SensorService::listOwners() {
  return repo_.findAll();
}

} // namespace rdws::sensor
