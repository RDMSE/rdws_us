//
// DeviceService — capabilities: device.list, device.get, device.create, device.update,
// device.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceRepository.h"
#include "../../shared/service/DeviceService.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/profiler.h"
#include "../../shared/utils/response_helper.h"

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
using namespace rdws::device;
using rdws::utils::ResponseHelper;
using json::JsonObj;

namespace {

rapidjson::Value deviceToJson(const Device& d, rapidjson::Document::AllocatorType& alloc) {
  JsonObj obj(alloc);
  obj.set("id", d.id)
      .set("field_id", d.fieldId)
      .set("type", d.type)
      .set("status", d.status);
  if (!d.installationDate.empty()) {
    obj.set("installation_date", d.installationDate);
  }
  if (!d.location.empty()) {
    obj.set("location", d.location);
  }
  obj.set("created_at", d.createdAt);
  if (!d.updatedAt.empty()) {
    obj.set("updated_at", d.updatedAt);
  }
  if (!d.updatedBy.empty()) {
    obj.set("updated_by", d.updatedBy);
  }
  return obj.take();
}

} // namespace

class AppDeviceService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // DB/repo/svc — declared in dependency order
  PostgreSQLDatabase db_;
  DeviceRepository repo_;
  rdws::device::DeviceService svc_;

public:
  AppDeviceService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), svc_(repo_) {
    identity.machineName = machineName;
    identity.serviceName = "device_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"device.list", "device.get", "device.create", "device.update",
                             "device.delete"};
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
    logger::info("DeviceService starting", identity.serviceId);
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
    logger::info("DeviceService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto& cap = json::getString(request, "capability").value_or("");
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<std::string,
                                    rdws::utils::CapabilityHandler<rdws::device::DeviceService>>
        handlers = {
            {"device.list", handleList},
            {"device.get", handleGet},
            {"device.create", handleCreate},
            {"device.update", handleUpdate},
            {"device.delete", handleDelete},
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
                                        rdws::device::DeviceService& svc) {
    const std::string fieldId =
        rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "field_id");
    const auto devices = svc.findAll(fieldId);
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& d : devices) {
        arr.PushBack(deviceToJson(d, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto device = svc.findById(id);
    if (!device) {
      return ResponseHelper::returnErrorDoc("Device not found", 404);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return deviceToJson(device.value(), alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const auto& fieldId = json::getString(req, "field_id").value_or(std::string{});
    const auto& type = json::getString(req, "type").value_or(std::string{});
    const auto& status = json::getString(req, "status").value_or(std::string{});

    if (fieldId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: field_id");
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type");
    }

    const std::string id = svc.create({.fieldId=fieldId, .type=type, .status=status});
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Failed to create device", 500);
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          return JsonObj(alloc).set("id", id).take();
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = json::getString(req, "type").value_or(std::string{});
    const std::string status = json::getString(req, "status").value_or(std::string{});

    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type");
    }
    if (status.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: status");
    }

    const bool ok = svc.update(id, {.type=type, .status=status});
    return ok ? ResponseHelper::returnSuccessDoc()
              : ResponseHelper::returnErrorDoc("Failed to update device", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const bool ok = svc.remove(id);
    return ok ? ResponseHelper::returnSuccessDoc(204)
              : ResponseHelper::returnErrorDoc("Failed to delete device", 500);
  }
};

static AppDeviceService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string serviceId = "device_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "device_dev";
    machineName = "dev-machine";
  }

  logger::init("device_service", "info", serviceId);

  AppDeviceService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize DeviceService");
    return 1;
  }
  service.run();
  return 0;
}
