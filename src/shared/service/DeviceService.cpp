#include "DeviceService.h"

namespace rdws::device {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

ServiceResult<std::vector<Device>> DeviceService::findAll(const std::string& fieldId) {
  if (!fieldId.empty() && !fieldRepo_.findById(fieldId)) {
    return ServiceResult<std::vector<Device>>::error("Field not found for id " + fieldId, 404);
  }
  return ServiceResult<std::vector<Device>>::success(repo_.findAll(fieldId));
}

std::optional<Device> DeviceService::findById(const std::string& id) {
  return repo_.findById(id);
}

ServiceResult<std::string> DeviceService::create(const DeviceCreate& data) {
  if (!fieldRepo_.findById(data.fieldId)) {
    return ServiceResult<std::string>::error("Field not found for id " + data.fieldId, 404);
  }
  const std::string id = repo_.create(data);
  if (id.empty()) {
    return ServiceResult<std::string>::error("Failed to create device", 500);
  }
  return ServiceResult<std::string>::success(id, 201);
}

OperationResult DeviceService::update(const std::string& id, const DeviceUpdate& data) {
  if (!repo_.findById(id)) {
    return OperationResult::error("Device not found for id " + id, 404);
  }
  // execCommand só reflete falha em erro de SQL/conexão — não em "0 linhas afetadas" —
  // por isso a checagem de existência acima é o que garante o 404 correto.
  if (!repo_.update(id, data)) {
    return OperationResult::error("Failed to update device", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

OperationResult DeviceService::remove(const std::string& id) {
  if (!repo_.findById(id)) {
    return OperationResult::error("Device not found for id " + id, 404);
  }
  if (!repo_.remove(id)) {
    return OperationResult::error("Failed to delete device", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Deleted"});
}

} // namespace rdws::device
