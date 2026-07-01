//
// AuthService — capability: auth.login
// Validates credentials against the users table and returns a JWT HS256 token.
//

#include "../../service_broker/Auth/AuthMiddleware.h"
#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/profiler.h"

#include <atomic>
#include <chrono>
#include "../../shared/utils/logger.h"

#include <csignal>
#include <cstdlib>
#include <fmt/core.h>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
namespace logger = rdws::utils::logger;

namespace {

std::string buildJwt(const std::string& userId, const std::string& username,
                     const std::string& role, const std::string& secret) {
  using namespace std::chrono;
  const int64_t now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  const int64_t exp = now + 86400; // 24h

  const std::string headerJson = R"({"alg":"HS256","typ":"JWT"})";

  rapidjson::Document payload;
  payload.SetObject();
  auto& alloc = payload.GetAllocator();
  payload.AddMember("sub", rapidjson::Value(userId.c_str(), alloc), alloc);
  payload.AddMember("username", rapidjson::Value(username.c_str(), alloc), alloc);
  payload.AddMember("role", rapidjson::Value(role.c_str(), alloc), alloc);
  payload.AddMember("iss", "rdws", alloc);
  payload.AddMember("iat", now, alloc);
  payload.AddMember("exp", exp, alloc);

  rapidjson::StringBuffer buf;
  rapidjson::Writer writer(buf);
  payload.Accept(writer);
  const std::string payloadJson = buf.GetString();

  const std::string hB64 = AuthMiddleware::base64urlEncode(
      reinterpret_cast<const unsigned char*>(headerJson.c_str()), headerJson.size());
  const std::string pB64 = AuthMiddleware::base64urlEncode(
      reinterpret_cast<const unsigned char*>(payloadJson.c_str()), payloadJson.size());

  const std::string signingInput = hB64 + "." + pB64;
  const std::string sig = AuthMiddleware::hmacSha256Base64url(secret, signingInput);
  return signingInput + "." + sig;
}

} // namespace

class AuthService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

public:
  AuthService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)) {
    identity.machineName = machineName;
    identity.serviceName = "auth_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"auth.login"};
  }

  bool initialize() {
    client = std::make_unique<ServiceClient>(identity, gatewayAddress);
    client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
      return processRequest(req);
    });
    return true;
  }

  void run() {
    running.store(true);
    logger::info("AuthService starting", identity.serviceId + " on " + gatewayAddress);
    while (running.load()) {
      client->run();
      if (!running.load()) {
        break;
      }
      logger::warn("Reconnecting in 3s", identity.serviceId);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      client = std::make_unique<servicegateway::ServiceClient>(identity, gatewayAddress);
      client->setRequestHandler(
          [this](const rapidjson::Document& req) { return processRequest(req); });
    }
    logger::info("AuthService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) const {
    const auto& cap = rdws::utils::json::getString(request, "capability").value_or("");
    logger::info("Dispatching capability", cap);

    if (cap == "auth.login") {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      return handleLogin(request);
    }

    return rdws::utils::ResponseHelper::returnErrorDoc("Unknown capability: " + cap, 404);
  }

  [[nodiscard]] rapidjson::Document handleLogin(const rapidjson::Document& request) const {
    logger::info("auth.login request received");
    // Extract credentials from top-level body fields (spread by HttpGateway)
    std::string username = rdws::utils::json::getString(request, "username").value_or("");
    std::string password = rdws::utils::json::getString(request, "password").value_or("");

    if (username.empty() || password.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("username and password are required", 400);
    }

    try {
      PostgreSQLDatabase db;

      // pgcrypto: crypt($2, password_hash) = password_hash
      auto rs = db.execQuery(
          "SELECT id, username, role FROM users "
          "WHERE username = $1 AND crypt($2, password_hash) = password_hash AND active = true",
          {username, password});

      if (!rs->next()) {
        return rdws::utils::ResponseHelper::returnErrorDoc("Invalid credentials", 401);
      }

      const std::string userId = rs->getString("id");
      const std::string uname = rs->getString("username");
      const std::string role = rs->getString("role");

      const char* secretEnv = std::getenv("JWT_SECRET");
      const std::string secret =
          (secretEnv != nullptr) ? secretEnv : "rdws-default-secret-change-me";
      const std::string token = buildJwt(userId, uname, role, secret);

      return rdws::utils::ResponseHelper::returnDataDoc([&](auto& alloc) {
        return rdws::utils::json::JsonObj(alloc)
          .set("token", token)
          .set("tokenType", "Bearer")
          .set("expiresIn", 86400)
          .set("user", rdws::utils::json::JsonObj(alloc)
            .set("id", userId)
            .set("username", uname)
            .set("role", role))
          .take();
      });
    } catch (const std::exception& e) {
      logger::error("DB error", identity.serviceId + " " + e.what());
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }
};

static AuthService* gService = nullptr;

void signalHandler(int sig) {
  if ((gService != nullptr) && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  std::string serviceId = "auth_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "auth_dev";
    machineName = "dev-machine";
  }

  logger::init("auth_service", "info", serviceId);

  AuthService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize AuthService");
    return 1;
  }
  service.run();
  return 0;
}
