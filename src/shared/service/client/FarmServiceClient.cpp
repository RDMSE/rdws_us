#include "FarmServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::farm {

bool FarmServiceClient::exists(const std::string& farmId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("FarmServiceClient: not connected to gateway", farmId);
    return false;
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
      return false;
    }
    logger::warn("FarmServiceClient: farm.get invoke failed", farmId + ": " + result.errorMessage);
    return false;
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the farm
  // payload — read "success" from it to know whether the farm was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("FarmServiceClient: invalid JSON envelope from farm.get", farmId);
    return false;
  }

  return json::getBool(envelope, "success").value_or(false);
}

} // namespace rdws::farm
