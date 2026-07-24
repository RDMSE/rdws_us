#include "config.h"

#include <dotenv.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rdws {

constexpr auto defaultServicesBasePath = "/opt/rdws/services";

Config::Config() {
  loadEnvironmentVariables();
}

std::optional<std::string> Config::get(const std::string& key) const {
  const auto it = settings.find(key);
  return it != settings.end() ? std::optional<std::string>{it->second} : std::nullopt;
}

void Config::set(const std::string& key, const std::string& value) {
  settings[key] = value;
}

std::string Config::getDatabaseHost() const {
  return get("DB_HOST").value_or("fedora-server");
}

std::string Config::getDatabasePort() const {
  return get("DB_PORT").value_or("5432");
}

std::string Config::getDatabaseName() const {
  return get("DB_NAME").value_or("");
}

std::string Config::getDatabaseUser() const {
  return get("DB_USER").value_or("");
}

std::string Config::getDatabasePassword() const {
  return get("DB_PASSWORD").value_or("");
}

std::string Config::getServicesBasePath() const {
  return get("SERVICES_BASE_PATH").value_or(defaultServicesBasePath);
}

std::uint32_t Config::getPort() const {
  return get("PORT").has_value() ? static_cast<std::uint32_t>(std::stoul(get("PORT").value()))
                                 : 8080;
}

std::string Config::getrLogLevel() const {
  return get("LOG_LEVEL").value_or("info");
}

std::string Config::getConnectionString() const {
  std::ostringstream oss;
  oss << "host=" << getDatabaseHost() << " port=" << getDatabasePort()
      << " dbname=" << getDatabaseName() << " user=" << getDatabaseUser()
      << " password=" << getDatabasePassword();
  return oss.str();
}

std::string Config::getEnvironment() const {
  return get("RDWS_ENVIRONMENT").value_or("development");
}

bool Config::isDevelopment() const {
  return getEnvironment() == "development";
}

bool Config::isProduction() const {
  return getEnvironment() == "production";
}

void Config::loadEnvironmentVariables() {
  // Also try generic .env file
  loadEnvFile("../../.env");

  // Load from environment variables
  settings["RDWS_ENVIRONMENT"] = getEnvVar("RDWS_ENVIRONMENT").value_or("test");
  settings["DB_PORT"] = getEnvVar("DB_PORT").value_or("1234");
  settings["DB_HOST"] = getEnvVar("DB_HOST").value_or("test-server");
  settings["DB_USER"] = getEnvVar("DB_USER").value_or("db_user");
  settings["DB_PASSWORD"] = getEnvVar("DB_PASSWORD").value_or("db_psswd");
  settings["DB_NAME"] =
      getEnvVar("DB_NAME").value_or("db_name"); // Will be set by getDatabaseName()
  settings["SERVICES_BASE_PATH"] =
      getEnvVar("SERVICES_BASE_PATH").value_or(defaultServicesBasePath);
  settings["LOG_LEVEL"] = getEnvVar("LOG_LEVEL").value_or("info");
  settings["PORT"] = getEnvVar("PORT").value_or("8080");
}

std::optional<std::string> Config::getEnvVar(const std::string& name) {
  const auto value = std::getenv(name.c_str());
  return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

std::string Config::getEnvVarOrDefault(const std::string& name, const std::string& def) {
  const auto value = getEnvVar(name);
  return (value && !value->empty()) ? *value : def;
}

void Config::loadEnvFile(const std::string& filename) {
  const auto filePath = (std::filesystem::current_path() / filename).string();
  dotenv::init(filePath.c_str());
}
} // namespace rdws
