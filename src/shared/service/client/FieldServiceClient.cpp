// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "FieldServiceClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::field {

FieldValidation FieldServiceClient::resolve(const std::string& fieldPublicId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("FieldServiceClient: not connected to gateway", fieldPublicId);
    return {.found = false, .statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value dataValue = json::JsonObj(allocator).set("public_id", fieldPublicId).take();
  data.Swap(dataValue);

  // field.ref, not field.get: it also returns the internal id
  const auto result = client_->invoke("field.ref", data);

  if (!result.success) {
    if (result.statusCode == 404) {
      return {.found = false, .statusCode = 404, .errorMessage = "Field not found"};
    }
    logger::warn("FieldServiceClient: field.ref invoke failed",
                 fieldPublicId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, etc.), not a "not found"
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.found = false, .statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope forwarded by the gateway is the whole handler response document
  // ({"success":..., "statusCode":..., "data"/"error":...}), not just the field
  // payload — read "success" from it to know whether the field was found.
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    logger::warn("FieldServiceClient: invalid JSON envelope from field.ref", fieldPublicId);
    return {.found = false, .statusCode = 502, .errorMessage = "Invalid JSON envelope from field.ref"};
  }

  if (json::getBool(envelope, "success").value_or(false)) {
    const auto* field = json::getObject(envelope, "data");
    const auto id = field ? json::getString(*field, "id") : std::nullopt;
    if (!id) {
      logger::warn("FieldServiceClient: field.ref response without id", fieldPublicId);
      return {.found = false, .statusCode = 502, .errorMessage = "field.ref response without id"};
    }
    return {.found = true, .id = *id};
  }

  const int envelopeStatus = json::getInt(envelope, "statusCode").value_or(404);
  return {.found = false, .statusCode = envelopeStatus, .errorMessage = "Field not found"};
}

} // namespace rdws::field
