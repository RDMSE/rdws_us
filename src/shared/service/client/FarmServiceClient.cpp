#include "FarmServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::farm {

FarmValidation FarmServiceClient::exists(const std::string& farmId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("FarmServiceClient: not connected to gateway", farmId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(allocator).set("id", farmId).take();
  rapidjson::Value dataValue =
      json::JsonObj(allocator).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const auto result = client_->invoke("farm.get", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Farm not found"};
    }
    logger::warn("FarmServiceClient: farm.get invoke failed", farmId + ": " + result.errorMessage);
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
    logger::warn("FarmServiceClient: invalid JSON envelope from farm.get", farmId);
    return {.found = false, .statusCode = 502, .errorMessage = "Invalid JSON envelope from farm.get"};
  }

  if (json::getBool(envelope, "success").value_or(false)) {
    return {.found = true};
  }

  const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
  return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Farm not found"};
}

} // namespace rdws::farm
