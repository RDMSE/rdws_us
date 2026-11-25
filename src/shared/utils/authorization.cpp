// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "authorization.h"

#include "json_helper.h"
#include "response_helper.h"

namespace rdws::utils {

std::optional<rapidjson::Document> requireAdmin(const rapidjson::Document& request) {
  if (json::hasActorIdentity(request) && json::getActorClaim(request, "role") != "admin") {
    return ResponseHelper::returnErrorDoc("Forbidden: admin role required", 403);
  }
  return std::nullopt;
}

std::optional<rapidjson::Document> requireInternal(const rapidjson::Document& request) {
  if (json::hasActorIdentity(request)) {
    return ResponseHelper::returnErrorDoc("Forbidden: internal capability", 403);
  }
  return std::nullopt;
}

} // namespace rdws::utils
