#pragma once

#include <string>

namespace rdws::field {

// Narrow interface used by domain services that only need to validate a field_id FK
// (list/create), without pulling in the rest of IFieldRepository's CRUD surface.
class IFieldValidator {
public:
  virtual ~IFieldValidator() = default;

  [[nodiscard]] virtual bool exists(const std::string& fieldId) = 0;
};

} // namespace rdws::field
