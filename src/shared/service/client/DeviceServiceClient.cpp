// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::device {

DeviceValidation DeviceServiceClient::resolve(const std::string& devicePublicId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("DeviceServiceClient: not connected to gateway", devicePublicId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value dataValue = json::JsonObj(allocator).set("public_id", devicePublicId).take();
  data.Swap(dataValue);

  // device.ref, not device.get: it also returns the internal id
  const auto result = client_->invoke("device.ref", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Device not found"};
    }
    logger::warn("DeviceServiceClient: device.ref invoke failed",
                 devicePublicId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, etc.), not a "not found"
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.found = false, .statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the device
  // payload — read "success" from it to know whether the device was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("DeviceServiceClient: invalid JSON envelope from device.ref", devicePublicId);
    return {.found = false, .statusCode = 502, .errorMessage = "Invalid JSON envelope from device.ref"};
  }

  if (json::getBool(envelope, "success").value_or(false)) {
    const auto* device = json::getObject(envelope, "data");
    const auto id = device ? json::getString(*device, "id") : std::nullopt;
    if (!id) {
      logger::warn("DeviceServiceClient: device.ref response without id", devicePublicId);
      return {.found = false, .statusCode = 502, .errorMessage = "device.ref response without id"};
    }
    return {.found = true, .id = *id};
  }

  const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
  return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Device not found"};
}

} // namespace rdws::device
