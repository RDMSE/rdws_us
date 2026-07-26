#include "DeviceConfigServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::device_config {

DeviceConfigResult DeviceConfigServiceClient::getConfig(const std::string& deviceId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("DeviceConfigServiceClient: not connected to gateway", deviceId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(allocator).set("id", deviceId).take();
  rapidjson::Value dataValue =
      json::JsonObj(allocator).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const auto result = client_->invoke("device_config.get", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Device config not found"};
    }
    logger::warn("DeviceConfigServiceClient: device_config.get invoke failed",
                deviceId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, etc.), not a "not found" -
    // surface the underlying status so callers don't misreport it as a 404.
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.found = false, .statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the config
  // payload — read "success"/"data.config" from it, mirroring FarmServiceClient/
  // FieldServiceClient.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("DeviceConfigServiceClient: invalid JSON envelope from device_config.get", deviceId);
    return {.found = false, .statusCode = 502,
            .errorMessage = "Invalid JSON envelope from device_config.get"};
  }

  if (!json::getBool(envelope, "success").value_or(false)) {
    const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
    return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Device config not found"};
  }

  const auto* dataObj = json::getObject(envelope, "data");
  if (dataObj == nullptr) {
    return {.found = false, .statusCode = 502,
            .errorMessage = "Missing data in device_config.get response"};
  }

  const auto configField = dataObj->FindMember("config");
  if (configField == dataObj->MemberEnd()) {
    return {.found = false, .statusCode = 502,
            .errorMessage = "Missing config field in device_config.get response"};
  }

  return {.found = true, .statusCode = 200, .configJson = json::docToString(configField->value)};
}

} // namespace rdws::device_config
