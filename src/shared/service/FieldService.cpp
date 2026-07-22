#include "FieldService.h"

namespace rdws::field {

using rdws::types::ServiceResult;

ServiceResult<std::vector<Field>> FieldService::findAll(const std::string& farmId) {
  if (!farmId.empty() && !farmValidator_.exists(farmId)) {
    return ServiceResult<std::vector<Field>>::error("Farm not found for id " + farmId, 404);
  }
  return ServiceResult<std::vector<Field>>::success(repo_.findAll(farmId));
}

std::optional<Field> FieldService::findById(const std::string& id) {
  return repo_.findById(id);
}

ServiceResult<std::string> FieldService::create(const FieldCreate& data) {
  if (data.farmId.empty()) {
    return ServiceResult<std::string>::error("Missing field: farm_id", 400);
  }
  if (!farmValidator_.exists(data.farmId)) {
    return ServiceResult<std::string>::error("Farm not found for id " + data.farmId, 404);
  }
  const std::string id = repo_.create(data);
  if (id.empty()) {
    return ServiceResult<std::string>::error("Failed to create field", 500);
  }
  return ServiceResult<std::string>::success(id, 201);
}

bool FieldService::update(const std::string& id, const FieldUpdate& data) {
  return repo_.update(id, data);
}

bool FieldService::remove(const std::string& id) {
  return repo_.remove(id);
}

} // namespace rdws::field
