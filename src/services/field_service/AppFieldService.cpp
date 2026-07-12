//
// FieldService — capabilities: field.list, field.get, field.create, field.update, field.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/FieldRepository.h"
#include "../../shared/service/FieldService.h"
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
namespace logger = rdws::utils::logger;
using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::field;
using rdws::utils::isNumericId;
using rdws::utils::ResponseHelper;
using json::JsonObj;

namespace {

rapidjson::Value fieldToJson(const Field& f, rapidjson::Document::AllocatorType& alloc) {
  JsonObj obj(alloc);
  obj.set("id", f.id)
    .set("farm_id", f.farmId)
    .set("name", f.name)
    .set("created_at", f.createdAt);
  if (!f.area.empty()) {
    obj.set("area", f.area);
  }
  if (!f.geometry.empty()) {
    obj.set("geometry", f.geometry);
  }
  if (!f.updatedAt.empty()) {
    obj.set("updated_at", f.updatedAt);
  }
  if (!f.updatedBy.empty()) {
    obj.set("updated_by", f.updatedBy);
  }
  return obj.take();
}


} // namespace

class AppFieldService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  FieldRepository repo_;
  rdws::field::FieldService svc_;

public:
  AppFieldService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "field_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"field.list", "field.get", "field.create", "field.update",
                             "field.delete"};
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
    logger::info("FieldService starting", identity.serviceId);
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
    logger::info("FieldService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const std::string cap = json::getString(request, "capability").value_or(std::string{});
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<std::string,
                                     rdws::utils::CapabilityHandler<rdws::field::FieldService>>
        handlers = {
            {"field.list",   handleList},
            {"field.get",    handleGet},
            {"field.create", handleCreate},
            {"field.update", handleUpdate},
            {"field.delete", handleDelete},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      return rdws::utils::dispatchCapability(cap, request, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(),
                                                         500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::field::FieldService& svc) {
    const std::string farmId = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "farm_id");
    if (!farmId.empty() && !isNumericId(farmId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: farm_id must be numeric", 400);
    }
    const auto fields = svc.findAll(farmId);
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& f : fields) {
        arr.PushBack(fieldToJson(f, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::field::FieldService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const auto field = svc.findById(id);
    if (!field) {
      return ResponseHelper::returnErrorDoc("Field not found", 404);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return fieldToJson(field.value(), alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    std::string farmId = json::getString(req, "farm_id").value_or(std::string{});
    std::string name = json::getString(req, "name").value_or(std::string{});

    if (farmId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: farm_id", 400);
    }
    if (!isNumericId(farmId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: farm_id must be numeric", 400);
    }
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    auto getArea = [&]() -> std::string {
      if (const auto valDouble = json::getDouble(req, "area")) {
        return std::to_string(valDouble.value());
      }
      if (const auto valStr = json::getString(req, "area")) {
        return valStr.value();
      }
      return "";
    };

    const FieldCreate data {
      .farmId = farmId,
      .name = name,
      .area = getArea()
    };

    const std::string id = svc.create(data);
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Failed to create field", 500);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          return JsonObj(alloc).set("id", id).take();
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    std::string name = json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: name", 400);
    }

    const bool ok = svc.update(id, {name});
    return ok ? ResponseHelper::returnSuccessDoc()
              : ResponseHelper::returnErrorDoc("Failed to update field", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const bool ok = svc.remove(id);
    return ok ? ResponseHelper::returnSuccessDoc(204)
              : ResponseHelper::returnErrorDoc("Failed to delete field", 500);
  }
};

static AppFieldService* gService = nullptr;

void signalHandler(const int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  std::string serviceId = "field_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "field_dev";
    machineName = "dev-machine";
  }

  logger::init("field_service", "info", serviceId);

  AppFieldService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize FieldService");
    return 1;
  }
  service.run();
  return 0;
}
