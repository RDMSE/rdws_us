#include "ServiceIdentity.h"
#include <algorithm>

namespace servicebroker {

Json::Value ServiceIdentity::toJson() const {
    Json::Value json;
    json["machineName"] = machineName;
    json["serviceName"] = serviceName;
    json["serviceId"] = serviceId;
    json["version"] = version;
    json["environment"] = environment;
    json["maxConcurrent"] = maxConcurrent;
    json["connectionType"] = connectionType;
    json["clientAddress"] = clientAddress;
    json["currentLoad"] = currentLoad;
    json["totalRequests"] = totalRequests;
    json["errorCount"] = errorCount;
    json["avgResponseTimeMs"] = static_cast<int>(avgResponseTime.count());
    
    // Capabilities array
    Json::Value capsArray(Json::arrayValue);
    for (const auto& cap : capabilities) {
        capsArray.append(cap);
    }
    json["capabilities"] = capsArray;
    
    // Timestamps (as seconds since epoch)
    auto connectedEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        connectedAt.time_since_epoch()).count();
    auto lastPingEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        lastPing.time_since_epoch()).count();
    
    json["connectedAt"] = static_cast<Json::Int64>(connectedEpoch);
    json["lastPing"] = static_cast<Json::Int64>(lastPingEpoch);
    
    return json;
}

ServiceIdentity ServiceIdentity::fromJson(const Json::Value& json) {
    ServiceIdentity identity;
    
    identity.machineName = json["machineName"].asString();
    identity.serviceName = json["serviceName"].asString();
    identity.serviceId = json["serviceId"].asString();
    identity.version = json["version"].asString();
    identity.environment = json.get("environment", "dev").asString();
    identity.maxConcurrent = json.get("maxConcurrent", 10).asUInt();
    identity.connectionType = json.get("connectionType", "").asString();
    identity.clientAddress = json.get("clientAddress", "").asString();
    identity.currentLoad = json.get("currentLoad", 0).asUInt();
    identity.totalRequests = json.get("totalRequests", 0).asUInt();
    identity.errorCount = json.get("errorCount", 0).asUInt();
    
    // Read capabilities
    if (const Json::Value& caps = json["capabilities"]; caps.isArray()) {
        for (const auto& cap : caps) {
            identity.capabilities.push_back(cap.asString());
        }
    }
    
    // Average response time
    const int responseTimeMs = json.get("avgResponseTimeMs", 0).asInt();
    identity.avgResponseTime = std::chrono::milliseconds(responseTimeMs);
    
    return identity;
}

bool ServiceIdentity::hasCapability(const std::string& capability) const {
    return std::ranges::find(capabilities, capability) != capabilities.end();
}

double ServiceIdentity::getLoadPercentage() const {
    if (maxConcurrent == 0) return 0.0;
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

} // namespace servicebroker