//
// SensorService — capabilities: sensor.list, sensor.get, sensor.create, sensor.update,
// sensor.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/SensorRepository.h"
#include "../../shared/service/SensorService.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/validation.h"

#include "../../shared/utils/logger.h"

#include <atomic>
#include <csignal>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::sensor;
using rdws::utils::isNumericId;
using rdws::utils::ResponseHelper;
namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

namespace {

rapidjson::Value sensorToJson(const Sensor& s, rapidjson::Document::AllocatorType& alloc) {
  json::JsonObj obj(alloc);
  obj.set("id", s.id)
      .set("device_id", s.deviceId)
      .set("type", s.type)
      .set("unit", s.unit)
      .set("created_at", s.createdAt);
  if (!s.location.empty()) {
    obj.set("location", s.location);
  }
  if (!s.updatedAt.empty()) {
    obj.set("updated_at", s.updatedAt);
  }
  if (!s.updatedBy.empty()) {
    obj.set("updated_by", s.updatedBy);
  }
  return obj.take();
}

} // namespace

class AppSensorService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  SensorRepository repo_;
  rdws::sensor::SensorService svc_;

public:
  AppSensorService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "sensor_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"sensor.list", "sensor.get", "sensor.create", "sensor.update",
                             "sensor.delete"};
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
    logger::info("SensorService starting", identity.serviceId);
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
    logger::info("SensorService stopped", identity.serviceId);
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

    static const std::unordered_map<std::string,
                                     rdws::utils::CapabilityHandler<rdws::sensor::SensorService>>
        handlers = {
            {"sensor.list", handleList},
            {"sensor.get", handleGet},
            {"sensor.create", handleCreate},
            {"sensor.update", handleUpdate},
            {"sensor.delete", handleDelete},
        };

    try {
      return rdws::utils::dispatchCapability(cap, request, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::sensor::SensorService& svc) {
    const std::string deviceId =
        rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "device_id");
    if (!deviceId.empty() && !isNumericId(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: device_id must be numeric", 400);
    }
    const auto sensors = svc.findAll(deviceId);
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& s : sensors) {
        arr.PushBack(sensorToJson(s, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const auto sensor = svc.findById(id);
    if (!sensor) {
      return ResponseHelper::returnErrorDoc("Sensor not found", 404);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return sensorToJson(*sensor, alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const auto deviceId = json::getString(req, "device_id").value_or(std::string{});
    const auto type = json::getString(req, "type").value_or(std::string{});
    const auto unit = json::getString(req, "unit").value_or(std::string{});

    if (deviceId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: device_id", 400);
    }
    if (!isNumericId(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: device_id must be numeric", 400);
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type", 400);
    }
    if (unit.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: unit", 400);
    }

    const SensorCreate data{
        .deviceId = deviceId,
        .type = type,
        .unit = unit,
        .updatedBy = json::getActorSubjectOrDefault(req)
    };
    const std::string id = svc.create(data);
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Failed to create sensor", 500);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          rapidjson::Value obj(rapidjson::kObjectType);
          obj.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
          return obj;
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = json::getString(req, "type").value_or(std::string{});
    const std::string unit = json::getString(req, "unit").value_or(std::string{});

    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type", 400);
    }
    if (unit.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: unit", 400);
    }

    const SensorUpdate data{
        .type = type,
        .unit = unit,
        .updatedBy = json::getActorSubjectOrDefault(req)
    };
    const bool ok = svc.update(id, data);
    return ok ? ResponseHelper::returnSuccessDoc()
              : ResponseHelper::returnErrorDoc("Failed to update sensor", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const bool ok = svc.remove(id);
    return ok ? ResponseHelper::returnSuccessDoc(204)
              : ResponseHelper::returnErrorDoc("Failed to delete sensor", 500);
  }
};

static AppSensorService* gService = nullptr;

void signalHandler(const int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  std::string serviceId = "sensor_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "sensor_dev";
    machineName = "dev-machine";
  }

  logger::init("sensor_service", "info", serviceId);

  AppSensorService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize SensorService");
    return 1;
  }
  service.run();
  return 0;
}
