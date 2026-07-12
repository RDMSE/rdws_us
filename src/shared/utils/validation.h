#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace rdws::utils {

// IDs use BIGINT in the schema. Invalid values can trigger "invalid input syntax for
// type bigint" in Postgres, resulting in a 500 instead of a 400 that explains the
// invalid field/parameter.
inline bool isNumericId(const std::string& value) {
  if (value.empty()) {
    return false;
  }

  const bool allAsciiDigits =
      std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (!allAsciiDigits) {
    return false;
  }

  constexpr std::string_view kMaxBigint = "9223372036854775807";
  const auto firstNonZero = value.find_first_not_of('0');
  const std::string_view normalized =
      (firstNonZero == std::string::npos) ? std::string_view{"0"} : std::string_view{value}.substr(firstNonZero);

  if (normalized.size() < kMaxBigint.size()) {
    return true;
  }
  if (normalized.size() > kMaxBigint.size()) {
    return false;
  }

  return normalized <= kMaxBigint;
}

} // namespace rdws::utils
