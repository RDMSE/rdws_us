// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

//
// SensorService — capabilities: sensor.list, sensor.get, sensor.create, sensor.update,
// sensor.delete, plus internal-only sensor.list_owners (not in routes.json; IngestionService's
// sensor_id -> {device_id, unit} cache, Plano_Telemetria.md Fase 0/DP1) and sensor.ref. The API
// addresses a sensor, and its device, by public id (UUID); the internal ids stay in the backend
// and in the station protocol (SenML names, the config's onboard[].sensor_id).
//

#include "../../shared/broker/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/SensorRepository.h"
#include "../../shared/service/SensorService.h"
#include "../../shared/service/client/DeviceServiceClient.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/profiler.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/validation.h"

#include "../../shared/utils/logger.h"
#include "rdws_version.h"

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
using rdws::utils::isUuid;
using rdws::utils::ResponseHelper;
namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

namespace {

rapidjson::Value sensorToJson(const Sensor& s, rapidjson::Document::AllocatorType& alloc) {
  json::JsonObj obj(alloc);
  obj.set("id", s.publicId)
      .set("device_id", s.devicePublicId)
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

  // DB/repo/svc — declared in dependency order. deviceValidator_ binds to `client` (still unset
  // at this point) and resolves device_id UUIDs via device_service through the gateway.
  PostgreSQLDatabase db_;
  SensorRepository repo_;
  rdws::device::DeviceServiceClient deviceValidator_;
  rdws::sensor::SensorService svc_;

public:
  AppSensorService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), deviceValidator_(client),
        svc_(repo_, deviceValidator_) {
    identity.machineName = machineName;
    identity.serviceName = "sensor_service";
    identity.serviceId = serviceId;
    identity.version = rdws::kVersion;
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"sensor.list",   "sensor.get",    "sensor.create",
                             "sensor.update", "sensor.delete", "sensor.list_owners",
                             "sensor.ref"};
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
            {"sensor.list_owners", handleListOwners},
            {"sensor.ref", handleRef},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      const rdws::utils::CapabilityContext ctx{request, profiler};
      return rdws::utils::dispatchCapability(cap, ctx, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc("Internal server error", 500);
    }
  }

  static rapidjson::Document handleList(const rdws::utils::CapabilityContext& ctx,
                                        rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    const std::string deviceId =
        rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "device_id");
    if (!deviceId.empty() && !isUuid(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: device_id must be a UUID", 400);
    }
    const auto result = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.findAll(deviceId);
    }();
    if (result.isError()) {
      return ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
    }

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& s : result.getData()) {
        arr.PushBack(sensorToJson(s, alloc), alloc);
      }
      return arr;
    });
  }

  // Bulk sensor_id -> {device_id, unit}, so IngestionService can drop readings for sensors that
  // don't belong to the device authenticated by the DTLS PSK, and convert SenML units to
  // sensors.unit (Plano_Telemetria.md, DP1).
  static rapidjson::Document handleListOwners(const rdws::utils::CapabilityContext& ctx,
                                              rdws::sensor::SensorService& svc) {
    const auto sensors = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.listOwners();
    }();

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& s : sensors) {
        arr.PushBack(json::JsonObj(alloc)
                         .set("id", s.id)
                         .set("device_id", s.deviceId)
                         .set("unit", s.unit)
                         .take(),
                     alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rdws::utils::CapabilityContext& ctx,
                                       rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be a UUID", 400);
    }

    const auto sensor = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.findByPublicId(id);
    }();
    if (!sensor) {
      return ResponseHelper::returnErrorDoc("Sensor not found", 404);
    }

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return sensorToJson(*sensor, alloc); });
  }

  static rapidjson::Document handleCreate(const rdws::utils::CapabilityContext& ctx,
                                          rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    const auto deviceId = json::getString(req, "device_id").value_or(std::string{});
    const auto type = json::getString(req, "type").value_or(std::string{});
    const auto unit = json::getString(req, "unit").value_or(std::string{});

    if (deviceId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: device_id", 400);
    }
    if (!isUuid(deviceId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: device_id must be a UUID", 400);
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type", 400);
    }
    if (unit.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: unit", 400);
    }

    const SensorCreate data{
        .devicePublicId = deviceId,
        .type = type,
        .unit = unit,
        .updatedBy = json::getActorSubjectOrDefault(req)
    };
    const auto result = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.create(data);
    }();
    if (result.isError()) {
      return ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
    }

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return json::JsonObj(alloc).set("id", result.getData()).take(); },
        201);
  }

  static rapidjson::Document handleUpdate(const rdws::utils::CapabilityContext& ctx,
                                          rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = json::getString(req, "type").value_or(std::string{});
    const std::string unit = json::getString(req, "unit").value_or(std::string{});

    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be a UUID", 400);
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
    auto t = ctx.profiler.scoped("db.query");
    const auto result = svc.update(id, data);
    return result.isSuccess()
               ? ResponseHelper::returnSuccessDoc()
               : ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
  }

  static rapidjson::Document handleDelete(const rdws::utils::CapabilityContext& ctx,
                                          rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be a UUID", 400);
    }

    auto t = ctx.profiler.scoped("db.query");
    const bool found = svc.remove(id);
    return found ? ResponseHelper::returnSuccessDoc(204)
                 : ResponseHelper::returnErrorDoc("Sensor not found", 404);
  }

  // Internal: both ids of a sensor (and of its device), by the internal "id" or by "public_id".
  // sensor_reading_service uses it to turn the path's sensor UUID into the internal id that
  // sensor_readings is keyed by. No routing rule exposes it (so HTTP /invoke refuses it), and
  // an HTTP caller's identity is refused here too, so internal ids can't leak to a client.
  static rapidjson::Document handleRef(const rdws::utils::CapabilityContext& ctx,
                                       rdws::sensor::SensorService& svc) {
    const auto& req = ctx.request;
    if (json::hasActorIdentity(req)) {
      return ResponseHelper::returnErrorDoc("Forbidden: internal capability", 403);
    }
    const std::string id = json::getString(req, "id").value_or(std::string{});
    const std::string publicId = json::getString(req, "public_id").value_or(std::string{});
    if (id.empty() == publicId.empty()) {
      return ResponseHelper::returnErrorDoc("Give exactly one of: id, public_id", 400);
    }
    if (!id.empty() && !isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid field: id must be numeric", 400);
    }
    if (!publicId.empty() && !isUuid(publicId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: public_id must be a UUID", 400);
    }

    const auto sensor = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return id.empty() ? svc.findByPublicId(publicId) : svc.findById(id);
    }();
    if (!sensor) {
      return ResponseHelper::returnErrorDoc("Sensor not found", 404);
    }
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      return json::JsonObj(alloc)
          .set("id", sensor->id)
          .set("public_id", sensor->publicId)
          .set("device_id", sensor->deviceId)
          .set("device_public_id", sensor->devicePublicId)
          .take();
    });
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
