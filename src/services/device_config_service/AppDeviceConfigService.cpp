//
// DeviceConfigService — capabilities: device_config.get, device_config.update
// Path parameter {id} refers to device_id.
//
// device_config is 1:1 with device (see Plano_DB_IOT_Sensors.md) — a default
// empty config ('{}') is created alongside the device via a trigger in Postgres
// (db/migrations/V3__device_config_one_to_one.sql), so there is no
// device_config.create nor device_config.delete: only GET (always exists) and UPDATE
// (merge patch — see src/shared/utils/json_merge.h).

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceConfigRepository.h"
#include "../../shared/service/DeviceConfigService.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/profiler.h"
#include "../../shared/utils/validation.h"

#include "../../shared/utils/logger.h"

#include <atomic>
#include <csignal>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

namespace json = rdws::utils::json;
using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::device_config;
using rdws::utils::isNumericId;
using rdws::utils::ResponseHelper;
using json::JsonObj;
namespace logger = rdws::utils::logger;

class AppDeviceConfigService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  DeviceConfigRepository repo_;
  rdws::device_config::DeviceConfigService svc_;

public:
  AppDeviceConfigService(const std::string& serviceId, const std::string& machineName,
                         std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "device_config_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"device_config.get", "device_config.update"};
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
    logger::info("DeviceConfigService starting", identity.serviceId);
    while (running.load()) {
      client->run();
      if (!running.load()) {
        break;
      }
      logger::warn("Reconnecting in 3s", identity.serviceId);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      client = std::make_unique<ServiceClient>(identity, gatewayAddress);
      client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
        return processRequest(req);
      });
    }
    logger::info("DeviceConfigService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {

    const auto cap = json::getString(request, "capability").value_or("");
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<
        std::string,
        rdws::utils::CapabilityHandler<rdws::device_config::DeviceConfigService>>
        handlers = {
            {"device_config.get", handleGet},
            {"device_config.update", handleUpdate},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);

      return rdws::utils::dispatchCapability(cap, request, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc("Internal server error", 500);
    }
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::device_config::DeviceConfigService& svc) {
    const std::string deviceId = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (deviceId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const auto cfg = svc.findByDeviceId(deviceId);
    if (!cfg) {
      return ResponseHelper::returnErrorDoc("Configuration not found", 404);
    }

    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      JsonObj obj(alloc);
      obj.set("id", cfg->id)
          .set("deviceId", cfg->deviceId)
          .setJsonOrString("config", cfg->config)
          .set("createdAt", cfg->createdAt);
      if (!cfg->updatedAt.empty()) {
        obj.set("updatedAt", cfg->updatedAt);
      }
      return obj.take();
    });
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::device_config::DeviceConfigService& svc) {
    const std::string deviceId = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (deviceId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    std::string configJson;
    if (const auto configField = req.FindMember("config"); configField != req.MemberEnd()) {
      configJson = json::docToString(configField->value);
    }
    if (configJson.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: config", 400);
    }

    const auto result = svc.update(deviceId, {configJson});
    return result.isSuccess()
               ? ResponseHelper::returnSuccessDoc()
               : ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
  }
};

static AppDeviceConfigService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string serviceId = "device_config_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "device_config_dev";
    machineName = "dev-machine";
  }

  logger::init("device_config_service", "info", serviceId);

  AppDeviceConfigService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize DeviceConfigService");
    return 1;
  }
  service.run();
  return 0;
}
