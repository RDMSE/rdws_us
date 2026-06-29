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
#include "../../shared/utils/response_helper.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::sensor;

namespace {

rapidjson::Value sensorToJson(const Sensor& s, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id", rapidjson::Value(s.id.c_str(), alloc), alloc);
  obj.AddMember("device_id", rapidjson::Value(s.deviceId.c_str(), alloc), alloc);
  obj.AddMember("type", rapidjson::Value(s.type.c_str(), alloc), alloc);
  obj.AddMember("unit", rapidjson::Value(s.unit.c_str(), alloc), alloc);
  if (!s.location.empty()) {
    obj.AddMember("location", rapidjson::Value(s.location.c_str(), alloc), alloc);
  }
  obj.AddMember("created_at", rapidjson::Value(s.createdAt.c_str(), alloc), alloc);
  if (!s.updatedAt.empty()) {
    obj.AddMember("updated_at", rapidjson::Value(s.updatedAt.c_str(), alloc), alloc);
  }
  if (!s.updatedBy.empty()) {
    obj.AddMember("updated_by", rapidjson::Value(s.updatedBy.c_str(), alloc), alloc);
  }
  return obj;
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
    identity.environment = "prod";
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
    std::cout << "[" << identity.serviceId << "] SensorService starting\n";
    while (running.load()) {
      client->run();
      if (!running.load()) {
        break;
      }
      std::cerr << "[" << identity.serviceId << "] Reconnecting in 3s...\n";
      std::this_thread::sleep_for(std::chrono::seconds(3));
      client = std::make_unique<ServiceClient>(identity, gatewayAddress);
      client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
        return processRequest(req);
      });
    }
    std::cout << "[" << identity.serviceId << "] SensorService stopped\n";
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto& cap = rdws::utils::getString(request, "capability").value_or("");
    std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

    try {
      if (cap == "sensor.list") {
        return handleList(request, svc_);
      }
      if (cap == "sensor.get") {
        return handleGet(request, svc_);
      }
      if (cap == "sensor.create") {
        return handleCreate(request, svc_);
      }
      if (cap == "sensor.update") {
        return handleUpdate(request, svc_);
      }
      if (cap == "sensor.delete") {
        return handleDelete(request, svc_);
      }

      return rdws::utils::ResponseHelper::returnErrorDoc("Unknown capability: " + cap, 404);
    } catch (const std::exception& e) {
      std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::sensor::SensorService& svc) {
    const std::string deviceId = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "device_id");
    const auto sensors = svc.findAll(deviceId);

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& s : sensors) {
      arr.PushBack(sensorToJson(s, alloc), alloc);
    }
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    const int total = static_cast<int>(arr.Size());
    doc.AddMember("data", arr, alloc);
    doc.AddMember("total", total, alloc);
    return doc;
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto sensor = svc.findById(id);
    if (!sensor) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Sensor not found", 404);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    doc.AddMember("data", sensorToJson(*sensor, alloc), alloc);
    return doc;
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const std::string deviceId = rdws::utils::getString(req, "device_id").value_or(std::string{});
    const std::string type = rdws::utils::getString(req, "type").value_or(std::string{});
    const std::string unit = rdws::utils::getString(req, "unit").value_or(std::string{});

    if (deviceId.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: device_id");
    }
    if (type.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: type");
    }
    if (unit.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: unit");
    }

    const std::string id = svc.create({deviceId, type, unit});
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Failed to create sensor", 500);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 201, alloc);
    rapidjson::Value data(rapidjson::kObjectType);
    data.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
    doc.AddMember("data", data, alloc);
    return doc;
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = rdws::utils::getString(req, "type").value_or(std::string{});
    const std::string unit = rdws::utils::getString(req, "unit").value_or(std::string{});

    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }
    if (type.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: type");
    }
    if (unit.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: unit");
    }

    const bool ok = svc.update(id, {type, unit});
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status",
                  ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc),
                  alloc);
    doc.AddMember("statusCode", ok ? 200 : 500, alloc);
    return doc;
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::sensor::SensorService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const bool ok = svc.remove(id);
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status",
                  ok ? rapidjson::Value("success", alloc) : rapidjson::Value("error", alloc),
                  alloc);
    doc.AddMember("statusCode", ok ? 204 : 500, alloc);
    return doc;
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

  AppSensorService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize SensorService\n";
    return 1;
  }
  service.run();
  return 0;
}
