// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace rdws::field {

// Result of resolving a field's public id. When found, id is the field's internal id (what a
// child row's FK stores). When found is false, statusCode carries the cause so callers can
// tell "field not found" (404) from transient failures (gateway disconnected, invoke
// timeout/503) instead of collapsing both into a 404.
struct FieldValidation {
  bool found = false;
  std::string id; // internal, set when found
  int statusCode = 404;
  std::string errorMessage;
};

// Narrow interface used by domain services that only need to check a field_id they received
// (a public id) and get the internal id their FK needs, without pulling in the rest of
// IFieldRepository's CRUD surface.
class IFieldValidator {
public:
  virtual ~IFieldValidator() = default;

  [[nodiscard]] virtual FieldValidation resolve(const std::string& fieldPublicId) = 0;
};

} // namespace rdws::field
