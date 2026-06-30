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

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <utility>

using namespace servicegateway;
using namespace rdws::database;
using namespace rdws::device;

namespace {

rapidjson::Value deviceToJson(const Device& d, rapidjson::Document::AllocatorType& alloc) {
  rapidjson::Value obj(rapidjson::kObjectType);
  obj.AddMember("id", rapidjson::Value(d.id.c_str(), alloc), alloc);
  obj.AddMember("field_id", rapidjson::Value(d.fieldId.c_str(), alloc), alloc);
  obj.AddMember("type", rapidjson::Value(d.type.c_str(), alloc), alloc);
  obj.AddMember("status", rapidjson::Value(d.status.c_str(), alloc), alloc);
  if (!d.installationDate.empty()) {
    obj.AddMember("installation_date", rapidjson::Value(d.installationDate.c_str(), alloc), alloc);
  }
  if (!d.location.empty()) {
    obj.AddMember("location", rapidjson::Value(d.location.c_str(), alloc), alloc);
  }
  obj.AddMember("created_at", rapidjson::Value(d.createdAt.c_str(), alloc), alloc);
  if (!d.updatedAt.empty()) {
    obj.AddMember("updated_at", rapidjson::Value(d.updatedAt.c_str(), alloc), alloc);
  }
  if (!d.updatedBy.empty()) {
    obj.AddMember("updated_by", rapidjson::Value(d.updatedBy.c_str(), alloc), alloc);
  }
  return obj;
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
    std::cout << "[" << identity.serviceId << "] DeviceService starting\n";
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
    std::cout << "[" << identity.serviceId << "] DeviceService stopped\n";
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
      std::cerr << "[" << identity.serviceId << "] error: " << e.what() << '\n';
      return rdws::utils::ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(),
                                                         500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::device::DeviceService& svc) {
    const std::string fieldId =
        rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "field_id");
    const auto devices = svc.findAll(fieldId);
    return rdws::utils::ResponseHelper::returnDataDoc([&](auto& alloc) {
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
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const auto device = svc.findById(id);
    if (!device) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Device not found", 404);
    }

    return rdws::utils::ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return deviceToJson(*device, alloc); });
  }

  static rapidjson::Document handleCreate(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const auto& fieldId = rdws::utils::getString(req, "field_id").value_or(std::string{});
    const auto& type = rdws::utils::getString(req, "type").value_or(std::string{});
    const auto& status = rdws::utils::getString(req, "status").value_or(std::string{});

    if (fieldId.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: field_id");
    }
    if (type.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: type");
    }

    const std::string id = svc.create({fieldId, type, status});
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Failed to create device", 500);
    }

    return rdws::utils::ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          rapidjson::Value data(rapidjson::kObjectType);
          data.AddMember("id", rapidjson::Value(id.c_str(), alloc), alloc);
          return data;
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = rdws::utils::getString(req, "type").value_or(std::string{});
    const std::string status = rdws::utils::getString(req, "status").value_or(std::string{});

    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }
    if (type.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: type");
    }
    if (status.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing field: status");
    }

    const bool ok = svc.update(id, {type, status});
    return ok ? rdws::utils::ResponseHelper::returnSuccessDoc()
              : rdws::utils::ResponseHelper::returnErrorDoc("Failed to update device", 500);
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return rdws::utils::ResponseHelper::returnErrorDoc("Missing path parameter: id");
    }

    const bool ok = svc.remove(id);
    return ok ? rdws::utils::ResponseHelper::returnSuccessDoc(204)
              : rdws::utils::ResponseHelper::returnErrorDoc("Failed to delete device", 500);
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

  AppDeviceService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "Failed to initialize DeviceService\n";
    return 1;
  }
  service.run();
  return 0;
}
