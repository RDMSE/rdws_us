#pragma once

#include <chrono>
#include <rapidjson/document.h>
#include <string>
#include <utility>
#include <vector>

namespace servicegateway {

struct ServiceIdentity {
  // Core identification
  std::string machineName;               // "server-01", "laptop-dev"
  std::string serviceName;               // "example_service", "greeting_service"
  std::string serviceId;                 // "srv_001", UUID único
  std::string version;                   // "v1.2.0"
  std::vector<std::string> capabilities; // ["greeting", "translation", "cache"]

  // Operational metadata
  std::string environment;    // "dev", "staging", "prod"
  uint32_t maxConcurrent{10}; // maximum simultaneos requests
  std::chrono::milliseconds avgResponseTime{0};

  // Connection info
  std::string connectionType; // "tcp", "unix"
  std::string clientAddress;  // IP ou path do socket
  std::chrono::time_point<std::chrono::steady_clock> connectedAt = std::chrono::steady_clock::now();

  // Runtime stats (updated during operation)
  uint32_t currentLoad = 0;   // Current active requests
  uint32_t totalRequests = 0; // Total processed requests
  uint32_t errorCount = 0;    // Total errors
  std::chrono::time_point<std::chrono::steady_clock> lastPing = std::chrono::steady_clock::now();

  // Constructors
  ServiceIdentity() = default;

  ServiceIdentity(std::string machine, std::string name, std::string id, std::string ver,
                  const std::vector<std::string>& caps = {}, std::string env = "dev",
                  const uint32_t maxConc = 10)
      : machineName(std::move(machine)), serviceName(std::move(name)), serviceId(std::move(id)),
        version(std::move(ver)), capabilities(caps), environment(std::move(env)),
        maxConcurrent(maxConc) {}

  // JSON serialization
  [[nodiscard]] rapidjson::Document toJson() const;
  [[nodiscard]] rapidjson::Value toJsonValue(rapidjson::Document::AllocatorType& allocator) const;
  static ServiceIdentity fromJson(const rapidjson::Value& json);

  // Utility methods
  [[nodiscard]] bool hasCapability(const std::string& capability) const;
  [[nodiscard]] double getLoadPercentage() const;
  [[nodiscard]] bool isOverloaded() const;
  [[nodiscard]] std::chrono::seconds getUptime() const;
  [[nodiscard]] bool isHealthy(std::chrono::seconds pingTimeout = std::chrono::seconds(60)) const;
};

} // namespace servicegateway
