//
// SensorReadingService — capabilities: sensor_reading.list, sensor_reading.get
// Append-only (read-only via REST — writes come from devices via PersistenceService).
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/SensorReadingRepository.h"
#include "../../shared/service/SensorReadingService.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::sensor_reading;

namespace {

rapidjson::Document makeError(const std::string& msg, int code = 400) {
  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();
  doc.AddMember("status", "error", alloc);
  doc.AddMember("statusCode", code, alloc);
  doc.AddMember("message", rapidjson::Value(msg.c_str(), alloc), alloc);
  return doc;
}

std::string getPathParam(const rapidjson::Document& req, const std::string& key) {
  if (req.HasMember("pathParameters") && req["pathParameters"].IsObject()) {
    const auto& pp = req["pathParameters"];
    if (pp.HasMember(key.c_str()) && pp[key.c_str()].IsString()) {
      return pp[key.c_str()].GetString();
    }
  }
  return {};
}

std::string getQueryParam(const rapidjson::Document& req, const std::string& key) {
  if (req.HasMember("queryStringParameters") && req["queryStringParameters"].IsObject()) {
    const auto& qp = req["queryStringParameters"];
    if (qp.HasMember(key.c_str()) && qp[key.c_str()].IsString()) {
      return qp[key.c_str()].GetString();
    }
  }
  return {};
}

rapidjson::Value readingToJson(const SensorReading& r, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id", rapidjson::Value(r.id.c_str(), alloc), alloc);
  obj.AddMember("sensor_id", rapidjson::Value(r.sensorId.c_str(), alloc), alloc);
  obj.AddMember("timestamp", rapidjson::Value(r.timestamp.c_str(), alloc), alloc);
  obj.AddMember("value", rapidjson::Value(r.value.c_str(), alloc), alloc);
  obj.AddMember("created_at", rapidjson::Value(r.createdAt.c_str(), alloc), alloc);
  return obj;
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
    std::cout << "[" << identity.serviceId << "] SensorReadingService starting\n";
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
    std::cout << "[" << identity.serviceId << "] SensorReadingService stopped\n";
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const std::string cap = request.HasMember("capability") && request["capability"].IsString()
                                ? request["capability"].GetString()
                                : "";
    std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

    try {
      if (cap == "sensor_reading.list") {
        return handleList(request, svc_);
      }
      if (cap == "sensor_reading.get") {
        return handleGet(request, svc_);
      }

      return makeError("Unknown capability: " + cap, 404);
    } catch (const std::exception& e) {
      std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
      return makeError(std::string("Internal error: ") + e.what(), 500);
    }
  }

  // GET /sensors/{id}/readings?from=...&to=...
  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::sensor_reading::SensorReadingService& svc) {
    const std::string sensorId = getPathParam(req, "id");
    if (sensorId.empty()) {
      return makeError("Missing path parameter: id");
    }

    const std::string from = getQueryParam(req, "from");
    const std::string to = getQueryParam(req, "to");

    const auto readings = svc.findBySensorId(sensorId, from, to);

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& r : readings) {
      arr.PushBack(readingToJson(r, alloc), alloc);
    }

    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    const int total = static_cast<int>(arr.Size());
    doc.AddMember("data", arr, alloc);
    doc.AddMember("total", total, alloc);
    return doc;
  }

  // GET /sensors/{id}/readings/{rid}
  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::sensor_reading::SensorReadingService& svc) {
    // 'rid' is the reading id; passed as a query or path param depending on routing
    std::string rid = getPathParam(req, "rid");
    if (rid.empty()) {
      rid = getQueryParam(req, "rid");
    }
    if (rid.empty()) {
      return makeError("Missing reading id (rid)");
    }

    const auto reading = svc.findById(rid);
    if (!reading) {
      return makeError("Reading not found", 404);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    doc.AddMember("data", readingToJson(*reading, alloc), alloc);
    return doc;
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

  AppSensorReadingService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize SensorReadingService\n";
    return 1;
  }
  service.run();
  return 0;
}
