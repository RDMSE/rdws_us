// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace rdws::farm {

// Result of resolving a farm's public id. When found, id is the farm's internal id (what a
// child row's FK stores). When found is false, statusCode carries the
// cause so callers can distinguish "farm not found" (404) from transient/operational
// failures (e.g. gateway disconnected, invoke timeout/503) instead of collapsing both
// into a 404.
struct FarmValidation {
  bool found = false;
  std::string id; // internal, set when found
  int statusCode = 404;
  std::string errorMessage;
};

// Narrow interface used by domain services that only need to check a farm_id they received
// (a public id) and get the internal id their FK needs, without pulling in the rest of
// IFarmRepository's CRUD surface.
class IFarmValidator {
public:
  virtual ~IFarmValidator() = default;

  [[nodiscard]] virtual FarmValidation resolve(const std::string& farmPublicId) = 0;
};

} // namespace rdws::farm
