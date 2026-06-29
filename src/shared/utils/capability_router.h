#pragma once

#include "response_helper.h"

#include <functional>
#include <rapidjson/document.h>
#include <string>
#include <unordered_map>

namespace rdws::utils {

template <typename TService>
using CapabilityHandler =
    std::function<rapidjson::Document(const rapidjson::Document&, TService&)>;

template <typename TService>
rapidjson::Document dispatchCapability(
    const std::string& cap, const rapidjson::Document& request, TService& svc,
    const std::unordered_map<std::string, CapabilityHandler<TService>>& handlers) {
  auto it = handlers.find(cap);
  if (it == handlers.end()) {
    return ResponseHelper::returnErrorDoc("Unknown capability: " + cap, 404);
  }
  return it->second(request, svc);
}

} // namespace rdws::utils
