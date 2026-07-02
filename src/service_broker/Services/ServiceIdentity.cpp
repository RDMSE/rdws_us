#include "ServiceIdentity.h"

#include "../../shared/utils/json_helper.h"

#include <algorithm>

namespace json = rdws::utils::json;

namespace servicegateway {

rapidjson::Document ServiceIdentity::toJson() const {
  rapidjson::Document json;
  json.SetObject();
  auto& allocator = json.GetAllocator();
  const rapidjson::Value value = toJsonValue(allocator);
  json.CopyFrom(value, allocator);
  return json;
}

rapidjson::Value ServiceIdentity::toJsonValue(rapidjson::Document::AllocatorType& allocator) const {
  rapidjson::Value capsArray(rapidjson::kArrayType);
  for (const auto& cap : capabilities) {
    capsArray.PushBack(rapidjson::Value(cap.c_str(), allocator), allocator);
  }

  const auto connectedEpoch =
    std::chrono::duration_cast<std::chrono::seconds>(connectedAt.time_since_epoch()).count();
  const auto lastPingEpoch =
    std::chrono::duration_cast<std::chrono::seconds>(lastPing.time_since_epoch()).count();

  rapidjson::Value json = json::JsonObj(allocator)
      .set("machineName", machineName)
      .set("serviceName", serviceName)
      .set("serviceId", serviceId)
      .set("version", version)
      .set("environment", environment)
      .set("maxConcurrent", maxConcurrent)
      .set("connectionType", connectionType)
      .set("clientAddress", clientAddress)
      .set("currentLoad", currentLoad)
      .set("totalRequests", totalRequests)
      .set("errorCount", errorCount)
      .set("avgResponseTimeMs", static_cast<int64_t>(avgResponseTime.count()))
      .setValue("capabilities", std::move(capsArray))
      .set("connectedAt", connectedEpoch)
      .set("lastPing", lastPingEpoch)
      .take();

  return json;
}

ServiceIdentity ServiceIdentity::fromJson(const rapidjson::Value& json) {
  ServiceIdentity identity;

  identity.machineName = json::getString(json, "machineName").value_or("");
  identity.serviceName = json::getString(json, "serviceName").value_or("");
  identity.serviceId = json::getString(json, "serviceId").value_or("");
  identity.version = json::getString(json, "version").value_or("");
  identity.environment = json::getString(json, "environment").value_or("dev");
  identity.maxConcurrent = json::getInt(json, "maxConcurrent").value_or(10);
  identity.connectionType = json::getString(json, "connectionType").value_or("");
  identity.clientAddress = json::getString(json, "clientAddress").value_or("");
  identity.currentLoad = json::getInt(json, "currentLoad").value_or(0);
  identity.totalRequests = json::getInt(json, "totalRequests").value_or(0);
  identity.errorCount = json::getInt(json, "errorCount").value_or(0);
  identity.avgResponseTime =
      std::chrono::milliseconds(json::getInt(json, "avgResponseTimeMs").value_or(0));

  // Read capabilities
  if (json.HasMember("capabilities") && json["capabilities"].IsArray()) {
    for (const auto& cap : json["capabilities"].GetArray()) {
      if (cap.IsString()) {
        identity.capabilities.emplace_back(cap.GetString());
      }
    }
  }

  return identity;
}

bool ServiceIdentity::hasCapability(const std::string& capability) const {
  return std::ranges::find(capabilities, capability) != capabilities.end();
}

double ServiceIdentity::getLoadPercentage() const {
  if (maxConcurrent == 0) {
    return 0.0;
  }
  return static_cast<double>(currentLoad) / static_cast<double>(maxConcurrent) * 100.0;
}

bool ServiceIdentity::isOverloaded() const {
  return currentLoad >= maxConcurrent;
}

std::chrono::seconds ServiceIdentity::getUptime() const {
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now - connectedAt);
}

bool ServiceIdentity::isHealthy(std::chrono::seconds pingTimeout) const {
  const auto now = std::chrono::steady_clock::now();
  const auto timeSinceLastPing = std::chrono::duration_cast<std::chrono::seconds>(now - lastPing);
  return timeSinceLastPing <= pingTimeout;
}

} // namespace servicegateway
