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

#include "../../shared/utils/logger.h"

#include <atomic>
#include <csignal>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::field;

namespace {

rapidjson::Value fieldToJson(const Field& f, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id", rapidjson::Value(f.id.c_str(), alloc), alloc);
  obj.AddMember("farm_id", rapidjson::Value(f.farmId.c_str(), alloc), alloc);
  obj.AddMember("name", rapidjson::Value(f.name.c_str(), alloc), alloc);
  if (!f.area.empty()) {
    obj.AddMember("area", rapidjson::Value(f.area.c_str(), alloc), alloc);
  }
  if (!f.geometry.empty()) {
    obj.AddMember("geometry", rapidjson::Value(f.geometry.c_str(), alloc), alloc);
  }
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
    rdws::logger::info("FieldService starting", identity.serviceId);
    while (running.load()) {
      client->run();
      if (!running.load()) {
        break;
      }
      rdws::logger::warn("Reconnecting in 3s", identity.serviceId);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      client = std::make_unique<ServiceClient>(identity, gatewayAddress);
      client->setRequestHandler([this](const rapidjson::Document& req) -> rapidjson::Document {
        return processRequest(req);
      });
    }
    rdws::logger::info("FieldService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const std::string cap = rdws::utils::json::getString(request, "capability").value_or(std::string{});
    rdws::logger::info("Dispatching capability", cap);

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
      rdws::logger::error("Request error", identity.serviceId + " " + e.what());
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(),
                                                         500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::field::FieldService& svc) {
    const std::string farmId = rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "farm_id");
    const auto fields = svc.findAll(farmId);
    return rdws::utils::ResponseHelper::returnDataDoc([&](auto& alloc) {
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
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto field = svc.findById(id);
    if (!field) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Field not found", 404);
    }

    return rdws::utils::ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return fieldToJson(*field, alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    std::string farmId = rdws::utils::json::getString(req, "farm_id").value_or(std::string{});
    std::string name = rdws::utils::json::getString(req, "name").value_or(std::string{});

    if (farmId.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: farm_id");
    }
    if (name.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: name");
    }

    auto getArea = [&]() -> std::string {
      if (auto val = rdws::utils::json::getDouble(req, "area")) {
        return std::to_string(val.value());
      } else if (auto val = rdws::utils::json::getString(req, "area")) {
        return val.value();
      }
      return "";
    };

    FieldCreate data {
      .farmId = farmId,
      .name = name,
      .area = getArea()
    };

    const std::string id = svc.create(data);
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Failed to create field", 500);
    }

    return rdws::utils::ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          rapidjson::Value obj(rapidjson::kObjectType);
          obj.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
          return obj;
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    std::string name = rdws::utils::json::getString(req, "name").value_or(std::string{});
    if (name.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: name");
    }

    const bool ok = svc.update(id, {name});
    return ok ? rdws::utils::ResponseHelper::returnSuccessDoc()
              : rdws::utils::ResponseHelper::returnErrorDoc("Failed to update field", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::field::FieldService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const bool ok = svc.remove(id);
    return ok ? rdws::utils::ResponseHelper::returnSuccessDoc(204)
              : rdws::utils::ResponseHelper::returnErrorDoc("Failed to delete field", 500);
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

  AppFieldService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    rdws::logger::error("Failed to initialize FieldService");
    return 1;
  }
  service.run();
  return 0;
}
