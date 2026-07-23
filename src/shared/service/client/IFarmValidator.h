#pragma once

#include <string>

namespace rdws::farm {

// Result of a farm_id existence check. When found is false, statusCode carries the
// cause so callers can distinguish "farm not found" (404) from transient/operational
// failures (e.g. gateway disconnected, invoke timeout/503) instead of collapsing both
// into a 404.
struct FarmValidation {
  bool found = false;
  int statusCode = 404;
  std::string errorMessage;
};

// Narrow interface used by domain services that only need to validate a farm_id FK
// (list/create), without pulling in the rest of IFarmRepository's CRUD surface.
class IFarmValidator {
public:
  virtual ~IFarmValidator() = default;

  [[nodiscard]] virtual FarmValidation exists(const std::string& farmId) = 0;
};

} // namespace rdws::farm
