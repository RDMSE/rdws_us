#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace rdws::utils {

// IDs BIGINT on schema; non numerical strings cause "invalid input syntax for type
// bigint" in Postgres, resulting in a 500 instead of a 400 explaining the invalid field/parameter.
inline bool isNumericId(const std::string& value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

} // namespace rdws::utils
