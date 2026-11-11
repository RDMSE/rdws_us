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
  // By the internal id, for other services (farm.ref)
  [[nodiscard]] std::optional<Farm> findById(const std::string& id);
  [[nodiscard]] std::optional<Farm> findByPublicId(const std::string& publicId);
  // 400 for a timezone PostgreSQL doesn't know; the new public id otherwise.
  [[nodiscard]] rdws::types::ServiceResult<std::string> create(const FarmCreate& data);
  // 404 when no farm has that public id
  [[nodiscard]] rdws::types::OperationResult update(const std::string& publicId,
                                                    const FarmUpdate& data);
  // false = no farm with that public id
  [[nodiscard]] bool remove(const std::string& publicId);

private:
  IFarmRepository& repo_;

  // "" when valid (empty = not given)
  [[nodiscard]] std::string checkTimezone(const std::string& timezone);
};

} // namespace rdws::farm
