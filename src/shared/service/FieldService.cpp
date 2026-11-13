// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FieldService.h"

namespace rdws::field {

using rdws::types::OperationResult;
using rdws::types::OperationStatus;
using rdws::types::ServiceResult;

rdws::farm::FarmValidation FieldService::resolveFarm(const std::string& farmPublicId) {
  auto validation = farmValidator_.resolve(farmPublicId);
  if (!validation.found) {
    validation.errorMessage = validation.statusCode == 404
                                  ? "Farm not found for id " + farmPublicId
                                  : "Farm validation failed for id " + farmPublicId + ": " +
                                        validation.errorMessage;
  }
  return validation;
}

ServiceResult<std::vector<Field>> FieldService::findAll(const std::string& farmPublicId) {
  if (!farmPublicId.empty()) {
    if (const auto farm = resolveFarm(farmPublicId); !farm.found) {
      return ServiceResult<std::vector<Field>>::error(farm.errorMessage, farm.statusCode);
    }
  }
  return ServiceResult<std::vector<Field>>::success(repo_.findAll(farmPublicId));
}

std::optional<Field> FieldService::findById(const std::string& id) {
  return repo_.findById(id);
}

std::optional<Field> FieldService::findByPublicId(const std::string& publicId) {
  return repo_.findByPublicId(publicId);
}

ServiceResult<std::string> FieldService::create(FieldCreate data) {
  if (data.farmPublicId.empty()) {
    return ServiceResult<std::string>::error("Missing field: farm_id", 400);
  }
  const auto farm = resolveFarm(data.farmPublicId);
  if (!farm.found) {
    return ServiceResult<std::string>::error(farm.errorMessage, farm.statusCode);
  }
  data.farmId = farm.id;
  const std::string id = repo_.create(data);
  if (id.empty()) {
    return ServiceResult<std::string>::error("Failed to create field", 500);
  }
  return ServiceResult<std::string>::success(id, 201);
}

OperationResult FieldService::update(const std::string& publicId, const FieldUpdate& data) {
  if (!repo_.update(publicId, data)) {
    return OperationResult::error("Field not found", 404);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

bool FieldService::remove(const std::string& publicId) {
  return repo_.remove(publicId);
}

} // namespace rdws::field
