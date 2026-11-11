// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

//
// FarmService — capabilities: farm.list, farm.get, farm.create, farm.update, farm.delete, and the
// internal farm.ref. The API addresses a farm by its public id (UUID); the internal id never
// leaves the backend, except to other services through farm.ref.
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
#include "../../shared/utils/validation.h"


#include "../../shared/utils/logger.h"
#include "rdws_version.h"

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
using rdws::utils::isNumericId;
using rdws::utils::isUuid;
using rdws::utils::ResponseHelper;
using json::JsonObj;

namespace {

std::string parseLocationWkt(const rapidjson::Document& req) {
  const auto* location = json::getObject(req, "location");
  if (location == nullptr) {
    return {};
  }
  const auto lat = json::getDouble(*location, "lat");
  const auto lng = json::getDouble(*location, "lng");
  if (!lat.has_value() || !lng.has_value()) {
    return {};
  }
  return "POINT(" + std::to_string(lng.value()) + " " + std::to_string(lat.value()) + ")";
}

rapidjson::Value farmToJson(const Farm& f, rapidjson::Document::AllocatorType& alloc) {
  JsonObj obj(alloc);
  obj.set("id", f.publicId)
    .set("name", f.name)
    .set("location", f.location)
    .set("timezone", f.timezone)
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
    identity.version = rdws::kVersion;
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"farm.list",   "farm.get",    "farm.create",
                             "farm.update", "farm.delete", "farm.ref"};
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
            {"farm.list",   handleList},
            {"farm.get",    handleGet},
            {"farm.create", handleCreate},
            {"farm.update", handleUpdate},
            {"farm.delete", handleDelete},
            {"farm.ref",    handleRef},
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
                                        rdws::farm::FarmService& svc) {
    const auto farms = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.findAll();
    }();

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& f : farms) {
        arr.PushBack(farmToJson(f, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rdws::utils::CapabilityContext& ctx,
                                       rdws::farm::FarmService& svc) {
    const auto& req = ctx.request;
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be a UUID", 400);
    }

    const auto farm = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.findByPublicId(id);
    }();
    if (!farm) {
      return ResponseHelper::returnErrorDoc("Farm not found", 404);
    }

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return farmToJson(farm.value(), alloc); });
  }

  static rapidjson::Document handleCreate(const rdws::utils::CapabilityContext& ctx,
                                          rdws::farm::FarmService& svc) {
    const auto& req = ctx.request;
    const std::string name = json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    const FarmCreate data{
        .name = name,
        .locationWkt = parseLocationWkt(req),
        .timezone = json::getString(req, "timezone").value_or(std::string{}),
        .updatedBy = json::getActorSubjectOrDefault(req)
    };

    const auto result = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.create(data);
    }();
    if (!result.isSuccess()) {
      return ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
    }

    auto t = ctx.profiler.scoped("json.serialize");
    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          return JsonObj(alloc).set("id", result.getData()).take();
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rdws::utils::CapabilityContext& ctx,
                                          rdws::farm::FarmService& svc) {
    const auto& req = ctx.request;
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be a UUID", 400);
    }

    const std::string name = json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    const FarmUpdate data{
        .name = name,
        .locationWkt = parseLocationWkt(req),
        .timezone = json::getString(req, "timezone").value_or(std::string{}),
        .updatedBy = json::getActorSubjectOrDefault(req)
    };

    auto t = ctx.profiler.scoped("db.query");
    const auto result = svc.update(id, data);
    return result.isSuccess()
               ? ResponseHelper::returnSuccessDoc()
               : ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
  }

  static rapidjson::Document handleDelete(const rdws::utils::CapabilityContext& ctx,
                                          rdws::farm::FarmService& svc) {
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
                 : ResponseHelper::returnErrorDoc("Farm not found", 404);
  }

  // Internal: both ids of a farm, looked up by the internal "id" or by "public_id". Other
  // services use it to check a farm_id they received and to get the id their FK needs. No
  // routing rule exposes it (so HTTP /invoke refuses it), and an HTTP caller's identity is
  // refused here too, so the internal id can't leak to a client.
  static rapidjson::Document handleRef(const rdws::utils::CapabilityContext& ctx,
                                       rdws::farm::FarmService& svc) {
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

    const auto farm = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return id.empty() ? svc.findByPublicId(publicId) : svc.findById(id);
    }();
    if (!farm) {
      return ResponseHelper::returnErrorDoc("Farm not found", 404);
    }
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      return JsonObj(alloc).set("id", farm->id).set("public_id", farm->publicId).take();
    });
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
