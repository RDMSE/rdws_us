#pragma once

#include "profiler.h"
#include "response_helper.h"

#include <functional>
#include <rapidjson/document.h>
#include <string>
#include <unordered_map>

namespace rdws::utils {

// Contexto passado a cada CapabilityHandler — carrega o request e o profiler
// do request corrente, permitindo medir sub-etapas do handler (db, serialize, etc.).
// Não confundir com rdws::types::RequestContext (metadados HTTP/API Gateway).
struct CapabilityContext {
  const rapidjson::Document& request;
  Profiler& profiler;
};

template <typename TService>
using CapabilityHandler =
    std::function<rapidjson::Document(const CapabilityContext&, TService&)>;

template <typename TService>
rapidjson::Document dispatchCapability(
    const std::string& cap,
    const CapabilityContext& ctx,
    TService& svc,
    const std::unordered_map<std::string, CapabilityHandler<TService>>& handlers) {
  auto it = handlers.find(cap);
  if (it == handlers.end()) {
    return ResponseHelper::returnErrorDoc("Unknown capability: " + cap, 404);
  }
  return it->second(ctx, svc);
}

} // namespace rdws::utils
