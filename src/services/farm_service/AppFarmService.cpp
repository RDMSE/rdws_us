//
// FarmService — capabilities: farm.list, farm.get, farm.create, farm.update, farm.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/FarmRepository.h"
#include "../../shared/service/FarmService.h"
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
#include <thread>
#include <utility>

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;
using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::farm;
using rdws::utils::ResponseHelper;
using json::JsonObj;

namespace {

rapidjson::Value farmToJson(const Farm& f, rapidjson::Document::AllocatorType& alloc) {
  JsonObj obj(alloc);
  obj.set("id", f.id)
    .set("name", f.name)
    .set("location", f.location)
    .set("created_at", f.createdAt);
  if (!f.updatedAt.empty()) {
    obj.set("updated_at", f.updatedAt);
  }
  if (!f.updatedBy.empty()) {
    obj.set("updated_by", f.updatedBy);
  }
  return obj.take();
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
    logger::info("FarmService starting", identity.serviceId);
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
    logger::info("FarmService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto cap = json::getString(request, "capability").value_or(std::string{});
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<std::string,
                                     rdws::utils::CapabilityHandler<rdws::farm::FarmService>>
        handlers = {
            {"farm.list",   [](const rapidjson::Document&, rdws::farm::FarmService& svc) { return handleList(svc); }},
            {"farm.get",    handleGet},
            {"farm.create", handleCreate},
            {"farm.update", handleUpdate},
            {"farm.delete", handleDelete},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      return rdws::utils::dispatchCapability(cap, request, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  static rapidjson::Document handleList(rdws::farm::FarmService& svc) {
    const auto farms = svc.findAll();
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& f : farms) {
        arr.PushBack(farmToJson(f, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::farm::FarmService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }

    const auto farm = svc.findById(id);
    if (!farm) {
      return ResponseHelper::returnErrorDoc("Farm not found", 404);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return farmToJson(farm.value(), alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::farm::FarmService& svc) {
    const std::string name = json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    FarmCreate data;
    data.name = name;
    if (const auto* location = json::getObject(req, "location"); location != nullptr) {
      const auto& loc = *location;
      const auto lat = json::getDouble(loc, "lat");
      const auto lng = json::getDouble(loc, "lng");
      if (lat.has_value() && lng.has_value()) {
        data.locationWkt =
            "POINT(" + std::to_string(lng.value()) + " " + std::to_string(lat.value()) + ")";
      }
    }

    const std::string id = svc.create(data);
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Failed to create farm", 500);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          return JsonObj(alloc).set("id", id).take();
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::farm::FarmService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }

    const std::string name = json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    FarmUpdate data;
    data.name = name;
    if (const auto* location = json::getObject(req, "location"); location != nullptr) {
      const auto& loc = *location;
      const auto lat = json::getDouble(loc, "lat");
      const auto lng = json::getDouble(loc, "lng");
      if (lat.has_value() && lng.has_value()) {
        data.locationWkt =
            "POINT(" + std::to_string(lng.value()) + " " + std::to_string(lat.value()) + ")";
      }
    }

    const bool ok = svc.update(id, data);
    return ok ? ResponseHelper::returnSuccessDoc()
              : ResponseHelper::returnErrorDoc("Failed to update farm", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::farm::FarmService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }

    const bool ok = svc.remove(id);
    return ok ? ResponseHelper::returnSuccessDoc(204)
              : ResponseHelper::returnErrorDoc("Failed to delete farm", 500);
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

  logger::init("farm_service", "info", serviceId);

  AppFarmService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize FarmService");
    return 1;
  }
  service.run();
  return 0;
}
