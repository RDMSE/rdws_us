#include "../shared/utils/logger.h"
#include "Auth/AuthMiddleware.h"
#include "HttpGateway.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <sstream>
#include <thread>

using namespace servicegateway;

namespace {

/// Read env var, returning empty string if unset or empty.
std::string getenv_str(const char* name) {
  const char* val = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
  return (val != nullptr) ? val : "";
}

/// Build an AuthConfig from the standard RDWS_AUTH_* environment variables.
///
/// RDWS_AUTH_MODE   = none | apikey | jwt   (default: none)
/// RDWS_API_KEYS    = key1=label1,key2=label2,...  (comma-separated, label optional)
/// RDWS_JWT_SECRET  = <hmac-sha256-secret>
/// RDWS_JWT_ISSUER  = <expected-iss-claim>   (optional)
/// RDWS_JWT_AUDIENCE= <expected-aud-claim>   (optional)
AuthConfig buildAuthConfig() {
  AuthConfig cfg;

  if (const std::string mode = getenv_str("RDWS_AUTH_MODE"); mode == "apikey") {
    cfg.mode = AuthMode::API_KEY;

    // Parse RDWS_API_KEYS="key1=label1,key2,key3=admin"
    if (const std::string raw = getenv_str("RDWS_API_KEYS"); !raw.empty()) {
      std::istringstream ss(raw);
      std::string token;
      while (std::getline(ss, token, ',')) {
        if (token.empty()) {
          continue;
        }
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
          cfg.apiKeys[token] = token; // label = key itself
        } else {
          cfg.apiKeys[token.substr(0, eq)] = token.substr(eq + 1);
        }
      }
    }
  } else if (mode == "jwt") {
    cfg.mode = AuthMode::JWT;
    cfg.jwtSecret = getenv_str("RDWS_JWT_SECRET");
    cfg.jwtIssuer = getenv_str("RDWS_JWT_ISSUER");
    cfg.jwtAudience = getenv_str("RDWS_JWT_AUDIENCE");
  }
  // else: NONE — no auth

  return cfg;
}

} // namespace

static ServiceGateway* g_serviceGateway = nullptr;
static HttpGateway* g_httpGateway = nullptr;

void signalHandler(const int signal) {
  rdws::logger::warn("Received signal, shutting down gateways", std::to_string(signal));

  if (g_httpGateway != nullptr) {
    g_httpGateway->stop();
  }

  if (g_serviceGateway != nullptr) {
    g_serviceGateway->stop();
  }

  std::signal(signal, SIG_DFL);
  std::raise(signal);
}

int main(const int argc, char* argv[]) {
  int brokerPort = 8080;
  int httpPort = 3001;
  std::string unixSocket = "/tmp/service_gateway.sock";
  std::string routesFile; // empty = no persistence
  // Config file: argv[5] or RDWS_CONFIG_FILE env var.
  std::string configFile = getenv_str("RDWS_CONFIG_FILE");

  if (argc >= 2) {
    brokerPort = std::stoi(argv[1]);
  }
  if (argc >= 3) {
    httpPort = std::stoi(argv[2]);
  }
  if (argc >= 4) {
    unixSocket = argv[3];
  }
  if (argc >= 5) {
    routesFile = argv[4];
  }
  if (argc >= 6) {
    configFile = argv[5];
  }

  // Logs to logs/rdws-gateway.log automatically — no CLI argument needed.
  rdws::logger::init("rdws-gateway", "info");

  const auto modeLabel = [&]() -> std::string {
    const std::string mode = getenv_str("RDWS_AUTH_MODE");
    if (mode == "apikey") return "apikey";
    if (mode == "jwt") return "jwt";
    return "none";
  }();

  rdws::logger::info("ServiceGateway HTTP Bridge starting",
    "broker=" + std::to_string(brokerPort) +
    " http=" + std::to_string(httpPort) +
    " unix=" + unixSocket +
    " auth=" + modeLabel +
    (routesFile.empty() ? "" : " routes=" + routesFile) +
    (configFile.empty() ? "" : " config=" + configFile));

  const AuthConfig authCfg = buildAuthConfig();

  try {
    ServiceGateway gateway(brokerPort, unixSocket, routesFile, configFile);
    HttpGateway httpGateway(gateway, httpPort, "0.0.0.0", authCfg);

    g_serviceGateway = &gateway;
    g_httpGateway = &httpGateway;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!gateway.start()) {
      rdws::logger::error("Failed to start ServiceGateway");
      return 1;
    }

    if (!httpGateway.start()) {
      rdws::logger::error("Failed to start HTTP gateway");
      gateway.stop();
      return 1;
    }

    rdws::logger::info("Gateway ready",
      "tcp=localhost:" + std::to_string(brokerPort) +
      " unix=" + unixSocket +
      " http=localhost:" + std::to_string(httpPort));

    while (gateway.isRunning() || httpGateway.isRunning()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  } catch (const std::exception& exception) {
    rdws::logger::error("Error starting HTTP gateway", exception.what());
    return 1;
  }

  return 0;
}
