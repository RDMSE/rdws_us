#include "DeviceService.h"

namespace rdws::device {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;

std::vector<Device> DeviceService::findAll(const std::string& fieldId) {
  return repo_.findAll(fieldId);
}

std::optional<Device> DeviceService::findById(const std::string& id) {
  return repo_.findById(id);
}

std::string DeviceService::create(const DeviceCreate& data) {
  return repo_.create(data);
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
