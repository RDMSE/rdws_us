#include "FieldService.h"

namespace rdws::field {

using rdws::types::ServiceResult;

ServiceResult<std::vector<Field>> FieldService::findAll(const std::string& farmId) {
  if (!farmId.empty()) {
    if (const auto validation = farmValidator_.exists(farmId); !validation.found) {
      return ServiceResult<std::vector<Field>>::error(
          validation.statusCode == 404 ? "Farm not found for id " + farmId
                                        : "Farm validation failed for id " + farmId + ": " +
                                              validation.errorMessage,
          validation.statusCode);
    }
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
  if (const auto validation = farmValidator_.exists(data.farmId); !validation.found) {
    return ServiceResult<std::string>::error(
        validation.statusCode == 404 ? "Farm not found for id " + data.farmId
                                      : "Farm validation failed for id " + data.farmId + ": " +
                                            validation.errorMessage,
        validation.statusCode);
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
