//
// DeviceService — capabilities: device.list, device.get, device.create, device.update,
// device.delete
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/config/config.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceRepository.h"
#include "../../shared/repository/FieldRepository.h"
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
#include <algorithm>
#include <cctype>
#include <rapidjson/document.h>
#include <regex>
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

// field_id/id são BIGINT no schema; string não-numérica causaria "invalid input syntax for
// type bigint" no Postgres, virando 500 em vez de um 400 explicando o campo inválido.
bool isNumericId(const std::string& value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// Aceita "YYYY-MM-DD" ou timestamp ISO 8601 completo (ex. "YYYY-MM-DDTHH:MM:SSZ"), validando
// não só o formato mas também faixas de mês/dia/hora/minuto/segundo (incluindo ano bissexto).
bool isValidInstallationDate(const std::string& value) {
  static const std::regex kPattern(
      R"(^(\d{4})-(\d{2})-(\d{2})([T ](\d{2}):(\d{2}):(\d{2})(\.\d+)?(Z|[+-]\d{2}:?\d{2})?)?$)");
  std::smatch m;
  if (!std::regex_match(value, m, kPattern)) {
    return false;
  }

  const int year = std::stoi(m[1]);
  const int month = std::stoi(m[2]);
  const int day = std::stoi(m[3]);
  if (month < 1 || month > 12) {
    return false;
  }
  static const int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = kDaysInMonth[month - 1];
  if (month == 2 && isLeapYear(year)) {
    maxDay = 29;
  }
  if (day < 1 || day > maxDay) {
    return false;
  }

  if (m[4].matched) {
    const int hour = std::stoi(m[5]);
    const int minute = std::stoi(m[6]);
    const int second = std::stoi(m[7]);
    if (hour > 23 || minute > 59 || second > 59) {
      return false;
    }

    if (m[9].matched && m[9].str() != "Z") {
      std::string tz = m[9].str(); // "+HH:MM", "+HHMM", "-HH:MM", "-HHMM"
      tz.erase(std::remove(tz.begin(), tz.end(), ':'), tz.end());
      const int offsetHour = std::stoi(tz.substr(1, 2));
      const int offsetMinute = std::stoi(tz.substr(3, 2));
      if (offsetHour > 23 || offsetMinute > 59) {
        return false;
      }
    }
  }
  return true;
}

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
  rdws::field::FieldRepository fieldRepo_; // valida field_id (FK) em list/create
  rdws::device::DeviceService svc_;

public:
  AppDeviceService(const std::string& serviceId, const std::string& machineName, std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), fieldRepo_(db_), svc_(repo_, fieldRepo_) {
    identity.machineName = machineName;
    identity.serviceName = "device_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {
        "device.list",
        "device.get",
        "device.create",
        "device.update",
        "device.delete"
    };
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
      return ResponseHelper::returnErrorDoc("Internal server error", 500);
    }
  }

  static rapidjson::Document handleList(const rapidjson::Document& req,
                                        rdws::device::DeviceService& svc) {
    const std::string fieldId =
        rdws::utils::LambdaParamsHelper::getStringQueryParam(req, "field_id");
    if (!fieldId.empty() && !isNumericId(fieldId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: field_id must be numeric", 400);
    }
    const auto result = svc.findAll(fieldId);
    if (result.isError()) {
      return ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
    }

    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value arr(rapidjson::kArrayType);
      for (const auto& d : result.getData()) {
        arr.PushBack(deviceToJson(d, alloc), alloc);
      }
      return arr;
    });
  }

  static rapidjson::Document handleGet(const rapidjson::Document& req,
                                       rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }

    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
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
    const auto fieldId = json::getString(req, "field_id").value_or(std::string{});
    const auto type = json::getString(req, "type").value_or(std::string{});
    const auto status = json::getString(req, "status").value_or(std::string{});

    if (fieldId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: field_id", 400);
    }
    if (!isNumericId(fieldId)) {
      return ResponseHelper::returnErrorDoc("Invalid field: field_id must be numeric", 400);
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type", 400);
    }

    const auto installationDate =
        json::getString(req, "installation_date").value_or(std::string{});
    if (!installationDate.empty() && !isValidInstallationDate(installationDate)) {
      return ResponseHelper::returnErrorDoc(
          "Invalid field: installation_date must be an ISO 8601 date or timestamp", 400);

    DeviceCreate data{.fieldId = fieldId, .type = type, .status = status};
    data.installationDate = installationDate;

    const auto result = svc.create(data);
    if (result.isError()) {
      return ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
    }

    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) {
          return JsonObj(alloc).set("id", result.getData()).take();
        },
        201);
  }

  static rapidjson::Document handleUpdate(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    const std::string type = json::getString(req, "type").value_or(std::string{});
    const std::string status = json::getString(req, "status").value_or(std::string{});

    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }
    if (type.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: type");
    }
    if (status.empty()) {
      return ResponseHelper::returnErrorDoc("Missing field: status");
    }

    const auto result = svc.update(id, {.type=type, .status=status});
    return result.isSuccess()
               ? ResponseHelper::returnSuccessDoc()
               : ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
  }

  static rapidjson::Document handleDelete(const rapidjson::Document& req,
                                          rdws::device::DeviceService& svc) {
    const std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty()) {
      return ResponseHelper::returnErrorDoc("Missing path parameter: id", 400);
    }
    if (!isNumericId(id)) {
      return ResponseHelper::returnErrorDoc("Invalid path parameter: id must be numeric", 400);
    }

    const auto result = svc.remove(id);
    return result.isSuccess()
               ? ResponseHelper::returnSuccessDoc(204)
               : ResponseHelper::returnErrorDoc(result.getErrorMessage(), result.getStatusCode());
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
