// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "DeviceDiagnosticsClient.h"

#include "../../broker/ServiceClient.h"
#include "../../utils/json_helper.h"
#include "../../utils/logger.h"

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace rdws::device {

using rdws::device_config::DeclaredConfig;
using rdws::device_config::DeclaredLookup;

using rdws::firmware::DeclaredFirmware;
using rdws::firmware::DeclaredFirmwareLookup;

std::unique_ptr<DeviceDiagnosticsClient::Snapshot>
DeviceDiagnosticsClient::snapshot(const std::string& deviceId) {
  auto result = std::make_unique<Snapshot>();
  const auto fail = [&result](int statusCode, std::string message) {
    result->statusCode = statusCode;
    result->errorMessage = std::move(message);
    return std::move(result);
  };
  if (!client_ || !client_->isConnected()) {
    logger::error("DeviceDiagnosticsClient: not connected to gateway", deviceId);
    return fail(503, "Not connected to gateway");
  }

  rapidjson::Document data;
  data.SetObject();
  auto& allocator = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(allocator).set("id", deviceId).take();
  rapidjson::Value dataValue =
      json::JsonObj(allocator).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const auto invoked = client_->invoke("device.diagnostics", data);
  if (!invoked.success) {
    logger::warn("DeviceDiagnosticsClient: device.diagnostics invoke failed",
                 deviceId + ": " + invoked.errorMessage);
    // Transient/operational failure (timeout, gateway error, ...): surface it, never "unknown"
    return fail(invoked.statusCode != 0 ? invoked.statusCode : 502, invoked.errorMessage);
  }

  // The envelope is the whole handler response ({"success", "statusCode", "data"/"error"})
  auto& envelope = result->envelope;
  envelope.Parse(invoked.responsePayload.c_str());
  if (envelope.HasParseError() || !envelope.IsObject()) {
    return fail(502, "Invalid JSON envelope from device.diagnostics");
  }
  if (!json::getBool(envelope, "success").value_or(false)) {
    return fail(json::getInt(envelope, "statusCode").value_or(502),
                json::getString(envelope, "error").value_or("device.diagnostics failed"));
  }
  result->body = json::getObject(envelope, "data");
  result->data = result->body != nullptr ? json::getObject(*result->body, "data") : nullptr;
  return result;
}

DeclaredLookup DeviceDiagnosticsClient::declared(const std::string& deviceId) {
  const auto s = snapshot(deviceId);
  if (s->statusCode != 200) {
    return {.statusCode = s->statusCode, .errorMessage = s->errorMessage};
  }
  // A snapshot without cfg_version (simulator) declares nothing, like no snapshot at all
  const auto version = s->data != nullptr ? json::getInt(*s->data, "cfg_version") : std::nullopt;
  if (!version) {
    return {};
  }
  return {.declared =
              DeclaredConfig{.declared = *version,
                             .rejected = json::getInt(*s->data, "cfg_rej"),
                             .declaredAt = json::getString(*s->body, "timestamp").value_or("")}};
}

DeclaredFirmwareLookup DeviceDiagnosticsClient::declaredFirmware(const std::string& deviceId) {
  const auto s = snapshot(deviceId);
  if (s->statusCode != 200) {
    return {.statusCode = s->statusCode, .errorMessage = s->errorMessage};
  }
  const auto build = s->data != nullptr ? json::getString(*s->data, "fw_build") : std::nullopt;
  if (!build) {
    return {};
  }
  return {.declared =
              DeclaredFirmware{.build = *build,
                               .version = json::getString(*s->data, "fw_version").value_or(""),
                               .boardClass = json::getString(*s->data, "fw_class"),
                               .rejected = json::getString(*s->data, "fw_rej"),
                               .percent = json::getInt(*s->data, "ota_pct"),
                               .declaredAt = json::getString(*s->body, "timestamp").value_or("")}};
}

} // namespace rdws::device
