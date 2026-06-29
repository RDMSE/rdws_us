//
// FarmService — capabilities: farm.list, farm.get, farm.create, farm.update, farm.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/FarmRepository.h"
#include "../../shared/service/FarmService.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/response_helper.h"


#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::farm;

namespace {

rapidjson::Value farmToJson(const Farm& f, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id", rapidjson::Value(f.id.c_str(), alloc), alloc);
  obj.AddMember("name", rapidjson::Value(f.name.c_str(), alloc), alloc);
  obj.AddMember("location", rapidjson::Value(f.location.c_str(), alloc), alloc);
  obj.AddMember("created_at", rapidjson::Value(f.createdAt.c_str(), alloc), alloc);
  if (!f.updatedAt.empty()) {
    obj.AddMember("updated_at", rapidjson::Value(f.updatedAt.c_str(), alloc), alloc);
  }
  if (!f.updatedBy.empty()) {
    obj.AddMember("updated_by", rapidjson::Value(f.updatedBy.c_str(), alloc), alloc);
  }
  return obj;
}

} // namespace

class AppFarmService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  FarmRepository repo_;
  rdws::farm::FarmService svc_;

public:
  AppFarmService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "farm_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"farm.list", "farm.get", "farm.create", "farm.update", "farm.delete"};
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
    std::cout << "[" << identity.serviceId << "] FarmService starting\n";
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
    std::cout << "[" << identity.serviceId << "] FarmService stopped\n";
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto& cap = rdws::utils::getString(request, "capability").value_or(std::string{});
    std::cout << "[" << identity.serviceId << "] capability=" << cap << '\n';

    try {
      if (cap == "farm.list") {
        return handleList(svc_);
      }
      if (cap == "farm.get") {
        return handleGet(request, svc_);
      }
      if (cap == "farm.create") {
        return handleCreate(request, svc_);
      }
      if (cap == "farm.update") {
        return handleUpdate(request, svc_);
      }
      if (cap == "farm.delete") {
        return handleDelete(request, svc_);
      }

      return rdws::utils::ResponseHelper::returnErrorDoc("Unknown capability: " + cap, 404);
    } catch (const std::exception& e) {
      std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  static rapidjson::Document handleList(rdws::farm::FarmService& svc) {
    const auto farms = svc.findAll();

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& f : farms) {
      arr.PushBack(farmToJson(f, alloc), alloc);
    }

    const int total = static_cast<int>(arr.Size());
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    doc.AddMember("data", arr, alloc);
    doc.AddMember("total", total, alloc);
    return doc;
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::farm::FarmService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto farm = svc.findById(id);
    if (!farm) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Farm not found", 404);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 200, alloc);
    doc.AddMember("data", farmToJson(*farm, alloc), alloc);
    return doc;
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::farm::FarmService& svc) {
    const auto& name = rdws::utils::getString(req, "name").value_or(std::string{});

    if (name.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: name");
    }

    FarmCreate data;
    data.name = name;
    if (const auto location = rdws::utils::getObject(req, "location"); location != nullptr) {
      const auto& loc = *location;
      const auto lat = rdws::utils::getDouble(loc, "lat");
      const auto lng = rdws::utils::getDouble(loc, "lng");
      if (lat.has_value() && lng.has_value()) {
        data.locationWkt =
            "POINT(" + std::to_string(lng.value()) + " " + std::to_string(lat.value()) + ")";
      }
    }

    const std::string id = svc.create(data);
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Failed to create farm", 500);
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("status", "success", alloc);
    doc.AddMember("statusCode", 201, alloc);
    rapidjson::Value dataObj(rapidjson::kObjectType);
    dataObj.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
    doc.AddMember("data", dataObj, alloc);
    return doc;
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::farm::FarmService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto& name = rdws::utils::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: name");
    }

    FarmUpdate data;
    data.name = name;
    if (const auto location = rdws::utils::getObject(req, "location"); location != nullptr) {
      const auto& loc = *location;
      const auto lat = rdws::utils::getDouble(loc, "lat");
      const auto lng = rdws::utils::getDouble(loc, "lng");
      if (lat.has_value() && lng.has_value()) {
        data.locationWkt =
            "POINT(" + std::to_string(lng.value()) + " " + std::to_string(lat.value()) + ")";
      }
    }

    const bool ok = svc.update(id, data);
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
                                          rdws::farm::FarmService& svc) {
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

static AppFarmService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string serviceId = "farm_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "farm_dev";
    machineName = "dev-machine";
  }

  AppFarmService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize FarmService\n";
    return 1;
  }
  service.run();
  return 0;
}
