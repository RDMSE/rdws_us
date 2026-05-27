#include "ServiceIdentity.h"
#include <algorithm>

namespace servicegateway {

rapidjson::Document ServiceIdentity::toJson() const {
    rapidjson::Document json;
    json.SetObject();
    auto &allocator = json.GetAllocator();
    const rapidjson::Value value = toJsonValue(allocator);
    json.CopyFrom(value, allocator);
    return json;
}

rapidjson::Value ServiceIdentity::toJsonValue(rapidjson::Document::AllocatorType& allocator) const {
    rapidjson::Value json(rapidjson::kObjectType);

    json.AddMember("machineName", rapidjson::Value(machineName.c_str(), allocator), allocator);
    json.AddMember("serviceName", rapidjson::Value(serviceName.c_str(), allocator), allocator);
    json.AddMember("serviceId", rapidjson::Value(serviceId.c_str(), allocator), allocator);
    json.AddMember("version", rapidjson::Value(version.c_str(), allocator), allocator);
    json.AddMember("environment", rapidjson::Value(environment.c_str(), allocator), allocator);
    json.AddMember("maxConcurrent", maxConcurrent, allocator);
    json.AddMember("connectionType", rapidjson::Value(connectionType.c_str(), allocator), allocator);
    json.AddMember("clientAddress", rapidjson::Value(clientAddress.c_str(), allocator), allocator);
    json.AddMember("currentLoad", currentLoad, allocator);
    json.AddMember("totalRequests", totalRequests, allocator);
    json.AddMember("errorCount", errorCount, allocator);
    json.AddMember("avgResponseTimeMs", static_cast<int64_t>(avgResponseTime.count()), allocator);

    rapidjson::Value capsArray(rapidjson::kArrayType);
    for (const auto& cap : capabilities) {
        capsArray.PushBack(rapidjson::Value(cap.c_str(), allocator), allocator);
    }
    json.AddMember("capabilities", capsArray, allocator);

    const auto connectedEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        connectedAt.time_since_epoch()).count();
    const auto lastPingEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        lastPing.time_since_epoch()).count();

    json.AddMember("connectedAt", connectedEpoch, allocator);
    json.AddMember("lastPing", lastPingEpoch, allocator);

    return json;
}

ServiceIdentity ServiceIdentity::fromJson(const rapidjson::Value& json) {
    ServiceIdentity identity;

    if (json.HasMember("machineName") && json["machineName"].IsString()) {
        identity.machineName = json["machineName"].GetString();
    }
    if (json.HasMember("serviceName") && json["serviceName"].IsString()) {
        identity.serviceName = json["serviceName"].GetString();
    }
    if (json.HasMember("serviceId") && json["serviceId"].IsString()) {
        identity.serviceId = json["serviceId"].GetString();
    }
    if (json.HasMember("version") && json["version"].IsString()) {
        identity.version = json["version"].GetString();
    }
    identity.environment = (json.HasMember("environment") && json["environment"].IsString())
                               ? json["environment"].GetString()
                               : "dev";
    identity.maxConcurrent = (json.HasMember("maxConcurrent") && json["maxConcurrent"].IsUint())
                                 ? json["maxConcurrent"].GetUint()
                                 : 10;
    identity.connectionType = (json.HasMember("connectionType") && json["connectionType"].IsString())
                                  ? json["connectionType"].GetString()
                                  : "";
    identity.clientAddress = (json.HasMember("clientAddress") && json["clientAddress"].IsString())
                                 ? json["clientAddress"].GetString()
                                 : "";
    identity.currentLoad = (json.HasMember("currentLoad") && json["currentLoad"].IsUint())
                               ? json["currentLoad"].GetUint()
                               : 0;
    identity.totalRequests = (json.HasMember("totalRequests") && json["totalRequests"].IsUint())
                                 ? json["totalRequests"].GetUint()
                                 : 0;
    identity.errorCount = (json.HasMember("errorCount") && json["errorCount"].IsUint())
                              ? json["errorCount"].GetUint()
                              : 0;

    // Read capabilities
    if (json.HasMember("capabilities") && json["capabilities"].IsArray()) {
        for (const auto& cap : json["capabilities"].GetArray()) {
            if (cap.IsString()) {
                identity.capabilities.emplace_back(cap.GetString());
            }
        }
    }

    // Average response time
    const int responseTimeMs = (json.HasMember("avgResponseTimeMs") && json["avgResponseTimeMs"].IsInt())
                                   ? json["avgResponseTimeMs"].GetInt()
                                   : 0;
    identity.avgResponseTime = std::chrono::milliseconds(responseTimeMs);

    return identity;
}

bool ServiceIdentity::hasCapability(const std::string& capability) const {
    return std::ranges::find(capabilities, capability) != capabilities.end();
}

double ServiceIdentity::getLoadPercentage() const {
    if (maxConcurrent == 0)
    {
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