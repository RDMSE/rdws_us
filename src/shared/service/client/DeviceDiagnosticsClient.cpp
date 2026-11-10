// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceDiagnosticsClient.h"

#include "../../../service_broker/Services/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::device {

using rdws::device_config::DeclaredConfig;
using rdws::device_config::DeclaredLookup;

DeclaredLookup DeviceDiagnosticsClient::declared(const std::string& deviceId) {
  if (!client_ || !client_->isConnected()) {
    logger::error("DeviceDiagnosticsClient: not connected to gateway", deviceId);
    return {.statusCode = 503, .errorMessage = "Not connected to gateway"};
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(allocator).set("id", deviceId).take();
  rapidjson::Value dataValue =
      json::JsonObj(allocator).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const auto result = client_->invoke("device.diagnostics", data);
  if (!result.success) {
    logger::warn("DeviceDiagnosticsClient: device.diagnostics invoke failed",
                 deviceId + ": " + result.errorMessage);
    // Transient/operational failure (timeout, gateway error, ...): surface it, never "unknown"
    const int statusCode = result.statusCode != 0 ? result.statusCode : 502;
    return {.statusCode = statusCode, .errorMessage = result.errorMessage};
  }

  // The envelope is the whole handler response ({"success", "statusCode", "data"/"error"})
  rapidjson::Document envelope;
  envelope.Parse(result.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    return {.statusCode = 502, .errorMessage = "Invalid JSON envelope from device.diagnostics"};
  }
  if (!json::getBool(envelope, "success").value_or(false)) {
    return {.statusCode = json::getInt(envelope, "statusCode").value_or(502),
            .errorMessage = json::getString(envelope, "error").value_or("device.diagnostics failed")};
  }

  // {"timestamp", "data"}: data is null when the device never sent diagnostics, and a snapshot
  // without cfg_version (simulator) declares nothing either
  const auto* body = json::getObject(envelope, "data");
  const auto* snapshot = body != nullptr ? json::getObject(*body, "data") : nullptr;
  const auto version = snapshot != nullptr ? json::getInt(*snapshot, "cfg_version") : std::nullopt;
  if (!version) {
    return {};
  }
  return {.declared = DeclaredConfig{.declared = *version,
                                     .rejected = json::getInt(*snapshot, "cfg_rej"),
                                     .declaredAt = json::getString(*body, "timestamp").value_or("")}};
}

} // namespace rdws::device
