// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FarmServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::farm {

FarmValidation FarmServiceClient::resolve(const std::string& farmPublicId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("FarmServiceClient: not connected to gateway", farmPublicId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value dataValue = json::JsonObj(allocator).set("public_id", farmPublicId).take();
  data.Swap(dataValue);

  // farm.ref, not farm.get: it also returns the internal id
  const auto result = client_->invoke("farm.ref", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Farm not found"};
    }
    logger::warn("FarmServiceClient: farm.ref invoke failed", farmPublicId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, etc.), not a "not found" -
    // surface the underlying status so callers don't misreport it as a 404.
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.found = false, .statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the farm
  // payload — read "success" from it to know whether the farm was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("FarmServiceClient: invalid JSON envelope from farm.ref", farmPublicId);
    return {.found = false, .statusCode = 502, .errorMessage = "Invalid JSON envelope from farm.ref"};
  }

  if (json::getBool(envelope, "success").value_or(false)) {
    const auto* farm = json::getObject(envelope, "data");
    const auto id = farm ? json::getString(*farm, "id") : std::nullopt;
    if (!id) {
      logger::warn("FarmServiceClient: farm.ref response without id", farmPublicId);
      return {.found = false, .statusCode = 502, .errorMessage = "farm.ref response without id"};
    }
    return {.found = true, .id = *id};
  }

  const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
  return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Farm not found"};
}

} // namespace rdws::farm
