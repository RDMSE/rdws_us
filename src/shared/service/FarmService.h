// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "../repository/FarmRepository.h"
#include "../types/service_result.h"

namespace rdws::farm {

class FarmService {
public:
  explicit FarmService(IFarmRepository& repo) : repo_(repo) {}

  [[nodiscard]] std::vector<Farm> findAll();
  [[nodiscard]] std::optional<Farm> findById(const std::string& id);
  // 400 for a timezone PostgreSQL doesn't know; the new id otherwise.
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(const FarmCreate& data);
  [[nodiscard]] rdws::types::OperationResult update(const std::string& id, const FarmUpdate& data);
  [[nodiscard]] bool remove(const std::string& id);

private:
  IFarmRepository& repo_;

  // "" when valid (empty = not given)
  [[nodiscard]] std::string checkTimezone(const std::string& timezone);
};

} // namespace rdws::farm
