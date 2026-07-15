#include "FieldServiceClient.h"

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::field {

bool FieldServiceClient::exists(const std::string& fieldId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("FieldServiceClient: not connected to gateway", fieldId);
    return false;
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(allocator).set("id", fieldId).take();
  rapidjson::Value dataValue =
      json::JsonObj(allocator).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const auto result = client_->invoke("field.get", data);

  if (!result.success) {
    logger::warn("FieldServiceClient: field.get invoke failed", fieldId + ": " + result.errorMessage);
    return false;
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the field
  // payload — read "success" from it to know whether the field was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    return false;
  }

  return json::getBool(envelope, "success").value_or(false);
}

} // namespace rdws::field
