//
// SensorReadingService — capabilities: sensor_reading.list, sensor_reading.get
// Append-only (read-only via REST — writes come from devices via PersistenceService).
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/SensorReadingRepository.h"
#include "../../shared/service/SensorReadingService.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/profiler.h"

#include "../../shared/utils/logger.h"

#include <atomic>
#include <csignal>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::sensor_reading;
namespace logger = rdws::utils::logger;

namespace {

rapidjson::Value readingToJson(const SensorReading& r, rapidjson::Document::AllocatorType& alloc) {
  return rdws::utils::json::JsonObj(alloc)
      .set("id", r.id)
      .set("sensor_id", r.sensorId)
      .set("timestamp", r.timestamp)
      .set("value", r.value)
      .set("created_at", r.createdAt)
      .take();
}

} // namespace

class AppSensorReadingService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  SensorReadingRepository repo_;
  rdws::sensor_reading::SensorReadingService svc_;

public:
  AppSensorReadingService(const std::string& serviceId, const std::string& machineName,
                          std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "sensor_reading_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 40;
    identity.capabilities = {"sensor_reading.list", "sensor_reading.get"};
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
    logger::info("SensorReadingService starting", identity.serviceId);
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
    logger::info("SensorReadingService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const std::string cap = rdws::utils::json::getString(request, "capability").value_or("");
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<
        std::string,
        rdws::utils::CapabilityHandler<rdws::sensor_reading::SensorReadingService>>
        handlers = {
            {"sensor_reading.list", handleList},
            {"sensor_reading.get", handleGet},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      return rdws::utils::dispatchCapability(cap, request, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  // GET /sensors/{id}/readings?from=...&to=...
  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::sensor_reading::SensorReadingService& svc) {
    const std::string sensorId = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (sensorId.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const std::string from = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "from");
    const std::string to = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "to");
    const auto readings = svc.findBySensorId(sensorId, from, to);

    return rdws::utils::ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& r : readings) {
        arr.PushBack(readingToJson(r, alloc), alloc);
      }
      return arr;
    });
  }

  // GET /sensors/{id}/readings/{rid}
  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::sensor_reading::SensorReadingService& svc) {
    std::string rid = rdws::utils::LambdaParamsHelper::getPathParam(req, "rid");
    if (rid.empty()) {
      rid = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "rid");
    }
    if (rid.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing reading id (rid)");
    }

    const auto reading = svc.findById(rid);
    if (!reading) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Reading not found", 404);
    }

    return rdws::utils::ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return readingToJson(reading.value(), alloc); });
  }
};

static AppSensorReadingService* gService = nullptr;

void signalHandler(const int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  std::string serviceId = "sensor_reading_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "sensor_reading_dev";
    machineName = "dev-machine";
  }

  logger::init("sensor_reading_service", "info", serviceId);

  AppSensorReadingService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize SensorReadingService");
    return 1;
  }
  service.run();
  return 0;
}
