//
// DeviceConfigService — capabilities: device_config.get, device_config.create,
//                                      device_config.update, device_config.delete
// Path parameter {id} refers to device_id.
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceConfigRepository.h"
#include "../../shared/service/DeviceConfigService.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::device_config;

namespace {

rapidjson::Document makeError(const std::string& msg, const int code = 400) {
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

} // namespace

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
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"device_config.get", "device_config.create", "device_config.update",
                             "device_config.delete"};
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
    std::cout << "[" << identity.serviceId << "] DeviceConfigService starting\n";
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
    std::cout << "[" << identity.serviceId << "] DeviceConfigService stopped\n";
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
      if (cap == "device_config.get") {
        return handleGet(request, svc_);
      }
      if (cap == "device_config.create") {
        return handleCreate(request, svc_);
      }
      if (cap == "device_config.update") {
        return handleUpdate(request, svc_);
      }
      if (cap == "device_config.delete") {
        return handleDelete(request, svc_);
      }
      return makeError("Unknown capability: " + cap, 404);
    } catch (const std::exception& e) {
      std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
      return makeError(std::string("Internal error: ") + e.what(), 500);
    }
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::device_config::DeviceConfigService& svc) {
    // {id} = device_id
    const std::string deviceId = getPathParam(req, "id");
    if (deviceId.empty()) {
      return makeError("Missing path parameter: id");
    }

    const auto cfg = svc.findByDeviceId(deviceId);
    if (!cfg) {
      return makeError("Configuration not found", 404);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    rapidjson::Value data(rapidjson::kObjectType);
    data.AddMember("id", rapidjson::Value(cfg->id.c_str(), alloc), alloc);
    data.AddMember("device_id", rapidjson::Value(cfg->deviceId.c_str(), alloc), alloc);

    // config is stored as JSONB — parse it back
    rapidjson::Document configDoc;
    if (!cfg->config.empty() && !configDoc.Parse(cfg->config.c_str()).HasParseError()) {
      rapidjson::Value configVal;
      configVal.CopyFrom(configDoc, alloc);
      data.AddMember("config", configVal, alloc);
    } else {
      data.AddMember("config", rapidjson::Value(cfg->config.c_str(), alloc), alloc);
    }

    data.AddMember("created_at", rapidjson::Value(cfg->createdAt.c_str(), alloc), alloc);
    if (!cfg->updatedAt.empty()) {
      data.AddMember("updated_at", rapidjson::Value(cfg->updatedAt.c_str(), alloc), alloc);
    }

    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    doc.AddMember("data", data, alloc);
    return doc;
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::device_config::DeviceConfigService& svc) {
    const std::string deviceId = getPathParam(req, "id");
    if (deviceId.empty()) {
      return makeError("Missing path parameter: id");
    }

    // Serialize the 'config' body field back to JSON string for JSONB
    std::string configJson;
    if (req.HasMember("config")) {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
      req["config"].Accept(writer);
      configJson = buf.GetString();
    }
    if (configJson.empty()) {
      return makeError("Missing field: config");
    }

    const std::string id = svc.create({deviceId, configJson});
    if (id.empty()) {
      return makeError("Failed to create configuration", 500);
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
                                          rdws::device_config::DeviceConfigService& svc) {
    const std::string deviceId = getPathParam(req, "id");
    if (deviceId.empty()) {
      return makeError("Missing path parameter: id");
    }

    std::string configJson;
    if (req.HasMember("config")) {
      rapidjson::StringBuffer buf;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
      req["config"].Accept(writer);
      configJson = buf.GetString();
    }
    if (configJson.empty()) {
      return makeError("Missing field: config");
    }

    const bool ok = svc.update(deviceId, {configJson});
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
                                          rdws::device_config::DeviceConfigService& svc) {
    const std::string deviceId = getPathParam(req, "id");
    if (deviceId.empty()) {
      return makeError("Missing path parameter: id");
    }

    const bool ok = svc.remove(deviceId);
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

  AppDeviceConfigService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize DeviceConfigService\n";
    return 1;
  }
  service.run();
  return 0;
}
