// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "SensorServiceClient.h"

#include "../../broker/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::sensor {

SensorValidation SensorServiceClient::resolve(const std::string& sensorPublicId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("SensorServiceClient: not connected to gateway", sensorPublicId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value dataValue = json::JsonObj(allocator).set("public_id", sensorPublicId).take();
  data.Swap(dataValue);

  // sensor.ref, not sensor.get: it also returns the internal id
  const auto result = client_->invoke("sensor.ref", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Sensor not found"};
    }
    logger::warn("SensorServiceClient: sensor.ref invoke failed",
                 sensorPublicId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, etc.), not a "not found"
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.found = false, .statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the sensor
  // payload — read "success" from it to know whether the sensor was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("SensorServiceClient: invalid JSON envelope from sensor.ref", sensorPublicId);
    return {.found = false, .statusCode = 502, .errorMessage = "Invalid JSON envelope from sensor.ref"};
  }

  if (json::getBool(envelope, "success").value_or(false)) {
    const auto* sensor = json::getObject(envelope, "data");
    const auto id = sensor ? json::getString(*sensor, "id") : std::nullopt;
    if (!id) {
      logger::warn("SensorServiceClient: sensor.ref response without id", sensorPublicId);
      return {.found = false, .statusCode = 502, .errorMessage = "sensor.ref response without id"};
    }
    return {.found = true, .id = *id};
  }

  const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
  return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Sensor not found"};
}

} // namespace rdws::sensor
