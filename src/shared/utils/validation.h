// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

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

// Public ids (Phase 13a) are UUIDs in the canonical 8-4-4-4-12 hex form, either case. Checked
// before the query for the same reason as isNumericId: "invalid input syntax for type uuid"
// would otherwise be a 500.
inline bool isUuid(std::string_view value) {
  constexpr size_t kLength = 36;
  if (value.size() != kLength) {
    return false;
  }
  for (size_t i = 0; i < kLength; ++i) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return false;
      }
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

} // namespace rdws::utils
