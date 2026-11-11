// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FarmService.h"

namespace rdws::farm {

std::vector<Farm> FarmService::findAll() {
  return repo_.findAll();
}

std::optional<Farm> FarmService::findById(const std::string& id) {
  return repo_.findById(id);
}

std::string FarmService::checkTimezone(const std::string& timezone) {
  if (timezone.empty() || repo_.isKnownTimezone(timezone)) {
    return {};
  }
  return "Unknown timezone: " + timezone + " (use an IANA name, e.g. America/Sao_Paulo)";
}

rdws::types::ServiceResult<std::string> FarmService::create(const FarmCreate& data) {
  using Result = rdws::types::ServiceResult<std::string>;
  if (const auto error = checkTimezone(data.timezone); !error.empty()) {
    return Result::error(error, 400);
  }
  const std::string id = repo_.create(data);
  if (id.empty()) {
    return Result::error("Failed to create farm", 500);
  }
  return Result::success(id, 201);
}

rdws::types::OperationResult FarmService::update(const std::string& id, const FarmUpdate& data) {
  using rdws::types::OperationResult;
  using rdws::types::OperationStatus;
  if (const auto error = checkTimezone(data.timezone); !error.empty()) {
    return OperationResult::error(error, 400);
  }
  if (!repo_.update(id, data)) {
    return OperationResult::error("Failed to update farm", 500);
  }
  return OperationResult::success(OperationStatus{.ok = true, .message = "Updated"});
}

bool FarmService::remove(const std::string& id) {
  return repo_.remove(id);
}

} // namespace rdws::farm
