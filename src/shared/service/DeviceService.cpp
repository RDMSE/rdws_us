// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceService.h"

namespace rdws::device {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

namespace {

std::string fieldError(const rdws::field::FieldValidation& v, const std::string& fieldPublicId) {
  return v.statusCode == 404
             ? "Field not found for id " + fieldPublicId
             : "Field validation failed for id " + fieldPublicId + ": " + v.errorMessage;
}

} // namespace

ServiceResult<std::vector<Device>> DeviceService::findAll(const std::string& fieldPublicId) {
  if (!fieldPublicId.empty()) {
    if (const auto field = fieldValidator_.resolve(fieldPublicId); !field.found) {
      return ServiceResult<std::vector<Device>>::error(fieldError(field, fieldPublicId),
                                                       field.statusCode);
    }
  }
  return ServiceResult<std::vector<Device>>::success(repo_.findAll(fieldPublicId));
}

std::optional<Device> DeviceService::findById(const std::string& id) {
  return repo_.findById(id);
}

std::optional<Device> DeviceService::findByPublicId(const std::string& publicId) {
  return repo_.findByPublicId(publicId);
}

ServiceResult<DeviceIds> DeviceService::create(DeviceCreate data) {
  if (data.fieldPublicId.empty()) {
    return ServiceResult<DeviceIds>::error("Missing field: field_id", 400);
  }
  const auto field = fieldValidator_.resolve(data.fieldPublicId);
  if (!field.found) {
    return ServiceResult<DeviceIds>::error(fieldError(field, data.fieldPublicId),
                                           field.statusCode);
  }
  data.fieldId = field.id;
  auto ids = repo_.create(data);
  if (ids.id.empty()) {
    return ServiceResult<DeviceIds>::error("Failed to create device", 500);
  }
  return ServiceResult<DeviceIds>::success(std::move(ids), 201);
}

OperationResult DeviceService::update(const std::string& publicId, const DeviceUpdate& data) {
  if (!repo_.update(publicId, data)) {
    return OperationResult::error("Device not found for id " + publicId, 404);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

OperationResult DeviceService::updateLocation(const std::string& id,
                                              const std::string& locationWkt) {
  if (!repo_.findById(id)) {
    return OperationResult::error("Device not found for id " + id, 404);
  }
  if (!repo_.updateLocation(id, locationWkt)) {
    return OperationResult::error("Failed to update device location", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

OperationResult DeviceService::remove(const std::string& publicId) {
  if (!repo_.remove(publicId)) {
    return OperationResult::error("Device not found for id " + publicId, 404);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Deleted"});
}

std::optional<rdws::device_telemetry::DiagnosticsSnapshot>
DeviceService::diagnostics(const Device& device) {
  return telemetry_.latestDiagnostics(device.id); // device_telemetry is keyed by the internal id
}

} // namespace rdws::device
