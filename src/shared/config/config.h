#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace rdws {

class Config {
private:
  std::map<std::string, std::string> settings;

public:
  Config();

  // Get configuration value
  [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

  // Set configuration value
  void set(const std::string& key, const std::string& value);

  // Database configuration
  [[nodiscard]] std::string getDatabaseHost() const;
  [[nodiscard]] std::string getDatabasePort() const;
  [[nodiscard]] std::string getDatabaseName() const;
  [[nodiscard]] std::string getDatabaseUser() const;
  [[nodiscard]] std::string getDatabasePassword() const;
  [[nodiscard]] std::string getConnectionString() const;
  [[nodiscard]] std::string getServicesBasePath() const;
  [[nodiscard]] std::uint32_t getPort() const;
  [[nodiscard]] std::string getrLogLevel() const;

  // Environment detection
  [[nodiscard]] std::string getEnvironment() const;
  [[nodiscard]] bool isDevelopment() const;
  [[nodiscard]] bool isProduction() const;

  // Reads an environment variable directly (bypassing the loaded settings map), falling
  // back to `def` when unset or empty. For service main()s reading their own runtime
  // config (host/port/credentials) rather than the shared settings loaded at construction.
  [[nodiscard]] static std::string getEnvVarOrDefault(const std::string& name, const std::string& def);

private:
  void loadEnvironmentVariables();
  static void loadEnvFile(const std::string& filename);
  [[nodiscard]] static std::optional<std::string> getEnvVar(const std::string& name);
};

} // namespace rdws
