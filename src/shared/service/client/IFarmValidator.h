#pragma once

#include <string>

namespace rdws::farm {

// Narrow interface used by domain services that only need to validate a farm_id FK
// (list/create), without pulling in the rest of IFarmRepository's CRUD surface.
class IFarmValidator {
public:
  virtual ~IFarmValidator() = default;

  [[nodiscard]] virtual bool exists(const std::string& farmId) = 0;
};

} // namespace rdws::farm
