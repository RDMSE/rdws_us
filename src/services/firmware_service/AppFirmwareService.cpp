// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

//
// FirmwareService — signed firmware images and which one each device should run (OTA,
// docs/Plano_OTA.md). Capabilities, all admin only over HTTP:
//   firmware_image.create / list / withdraw
//   firmware_target.set / retry, firmware_ota.set, firmware.status
// Path parameter {id} is a public id (UUID): the image's or the device's. No route reaches more than
// one device (O17).
//
// The image files come from a read-only volume (FIRMWARE_IMAGES_DIR), filled by the publish script.
// Delivering them to the stations is the next phase.

#include "../../shared/broker/ServiceClient.h"
#include "../../shared/config/config.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/firmware/file_image_store.h"
#include "../../shared/repository/FirmwareRepository.h"
#include "../../shared/service/FirmwareService.h"
#include "../../shared/service/client/DeviceDiagnosticsClient.h"
#include "../../shared/utils/authorization.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/logger.h"
#include "../../shared/utils/profiler.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/validation.h"
#include "rdws_version.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;
using namespace servicegateway;
using namespace rdws::firmware;
using json::JsonObj;
using rdws::utils::CapabilityContext;
using rdws::utils::isUuid;
using rdws::utils::ResponseHelper;

namespace {

std::string imagesDirectory() {
  const char* dir = std::getenv("FIRMWARE_IMAGES_DIR"); // NOLINT(concurrency-mt-unsafe)
  return dir != nullptr && *dir != '\0' ? dir : "/app/firmware";
}

} // namespace

class AppFirmwareService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // Declared in dependency order
  rdws::database::PostgreSQLDatabase db_;
  FirmwareRepository repo_;
  FileImageStore store_;
  // Binds to `client` (still unset here): asks device_service, through the gateway
  rdws::device::DeviceDiagnosticsClient declaredSource_;
  FirmwareService svc_;

public:
  AppFirmwareService(const std::string& serviceId, const std::string& machineName,
                     std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), store_(imagesDirectory()),
        declaredSource_(client), svc_(repo_, store_, declaredSource_) {
    identity.machineName = machineName;
    identity.serviceName = "firmware_service";
    identity.serviceId = serviceId;
    identity.version = rdws::version();
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"firmware_image.create", "firmware_image.list",
                             "firmware_image.withdraw", "firmware_target.set",
                             "firmware_target.retry",  "firmware_ota.set",
                             "firmware.status"};
  }

  void run() {
    running.store(true);
    logger::info("FirmwareService starting", identity.serviceId + " images=" + imagesDirectory());
    while (running.load()) {
      connect();
      client->run();
      if (!running.load()) {
        break;
      }
      logger::warn("Reconnecting in 3s", identity.serviceId);
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    logger::info("FirmwareService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  void connect() {
    client = std::make_unique<ServiceClient>(identity, gatewayAddress);
    client->setRequestHandler(
        [this](const rapidjson::Document& req) { return processRequest(req); });
  }

  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto cap = json::getString(request, "capability").value_or("");
    logger::info("Dispatching capability", cap);

    static const std::unordered_map<std::string, rdws::utils::CapabilityHandler<FirmwareService>>
        handlers = {
            {"firmware_image.create", handleImageCreate},
            {"firmware_image.list", handleImageList},
            {"firmware_image.withdraw", handleImageWithdraw},
            {"firmware_target.set", handleTargetSet},
            {"firmware_target.retry", handleTargetRetry},
            {"firmware_ota.set", handleOtaSet},
            {"firmware.status", handleStatus},
        };

    // Before any handler: none of them is for anyone else
    if (auto denied = rdws::utils::requireAdmin(request)) {
      return std::move(*denied);
    }
    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      const CapabilityContext ctx{request, profiler};
      return rdws::utils::dispatchCapability(cap, ctx, svc_, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc("Internal server error", 500);
    }
  }

  // The path's {id}, or the error response
  [[nodiscard]] static std::variant<std::string, rapidjson::Document>
  pathId(const rapidjson::Document& req) {
    std::string id = rdws::utils::LambdaParamsHelper::getPathParam(req, "id");
    if (id.empty() || !isUuid(id)) {
      return ResponseHelper::returnErrorDoc("Missing/invalid path parameter: id must be a UUID",
                                            400);
    }
    return id;
  }

  template <typename Result> [[nodiscard]] static rapidjson::Document errorOf(const Result& r) {
    return ResponseHelper::returnErrorDoc(r.getErrorMessage(), r.getStatusCode());
  }

  template <typename Alloc>
  [[nodiscard]] static rapidjson::Value imageJson(const FirmwareImage& image, Alloc& alloc) {
    JsonObj obj(alloc);
    obj.set("id", image.publicId)
        .set("buildId", image.buildId)
        .set("sha256", image.sha256)
        .set("size", image.sizeBytes)
        .set("boardClass", image.boardClass)
        .set("version", image.version)
        .set("variant", image.variant)
        .set("notes", image.notes)
        .set("createdAt", image.createdAt)
        .set("createdBy", image.createdBy);
    if (image.withdrawnAt.empty()) {
      obj.setNull("withdrawnAt");
    } else {
      obj.set("withdrawnAt", image.withdrawnAt);
    }
    return obj.take();
  }

  static rapidjson::Document handleImageCreate(const CapabilityContext& ctx,
                                               FirmwareService& svc) {
    const auto& req = ctx.request;
    const auto result = [&] {
      auto t = ctx.profiler.scoped("register");
      return svc.registerImage(json::getString(req, "sha256").value_or(""),
                               json::getString(req, "variant").value_or(""),
                               json::getString(req, "notes").value_or(""),
                               json::getActorSubjectOrDefault(req));
    }();
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    logger::info("firmware image registered", result.getData().buildId + " by " +
                                                  result.getData().createdBy);
    return ResponseHelper::returnDataDoc(
        [&](auto& alloc) { return imageJson(result.getData(), alloc); }, 201);
  }

  static rapidjson::Document handleImageList(const CapabilityContext& ctx, FirmwareService& svc) {
    const auto images = [&] {
      auto t = ctx.profiler.scoped("db.query");
      return svc.images();
    }();
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      rapidjson::Value list(rapidjson::kArrayType);
      for (const auto& image : images) {
        auto item = imageJson(image, alloc);
        rapidjson::Value devices(rapidjson::kArrayType);
        for (const auto& device : image.targetedBy) {
          devices.PushBack(rapidjson::Value(device.c_str(), alloc), alloc);
        }
        item.AddMember("targetedBy", devices, alloc);
        list.PushBack(item, alloc);
      }
      return list;
    });
  }

  static rapidjson::Document handleImageWithdraw(const CapabilityContext& ctx,
                                                 FirmwareService& svc) {
    auto id = pathId(ctx.request);
    if (auto* error = std::get_if<rapidjson::Document>(&id)) {
      return std::move(*error);
    }
    const auto by = json::getActorSubjectOrDefault(ctx.request);
    const auto result = svc.withdraw(std::get<std::string>(id), by);
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    logger::info("firmware image withdrawn", std::get<std::string>(id) + " by " + by);
    return ResponseHelper::returnSuccessDoc();
  }

  // {"imageId": "<uuid>" | null, "allowBench": bool}. imageId is required, so that a body without it
  // never clears a target by accident.
  static rapidjson::Document handleTargetSet(const CapabilityContext& ctx, FirmwareService& svc) {
    const auto& req = ctx.request;
    auto id = pathId(req);
    if (auto* error = std::get_if<rapidjson::Document>(&id)) {
      return std::move(*error);
    }
    const auto field = req.FindMember("imageId");
    if (field == req.MemberEnd()) {
      return ResponseHelper::returnErrorDoc("Missing field: imageId (null clears the target)", 400);
    }
    std::optional<std::string> imageId;
    if (!field->value.IsNull()) {
      if (!field->value.IsString() || !isUuid(field->value.GetString())) {
        return ResponseHelper::returnErrorDoc("Invalid field: imageId must be a UUID or null", 400);
      }
      imageId = field->value.GetString();
    }
    const auto& deviceId = std::get<std::string>(id);
    const auto by = json::getActorSubjectOrDefault(req);
    const bool allowBench = json::getBool(req, "allowBench").value_or(false);
    const auto result = svc.setTarget(deviceId, imageId, allowBench, by);
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    logger::info("firmware target set", deviceId + " image=" + imageId.value_or("none") +
                                            " allowBench=" + (allowBench ? "true" : "false") +
                                            " by " + by);
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      JsonObj obj(alloc);
      if (result.getData().warning.empty()) {
        obj.setNull("warning");
      } else {
        obj.set("warning", result.getData().warning);
      }
      return obj.take();
    });
  }

  static rapidjson::Document handleTargetRetry(const CapabilityContext& ctx, FirmwareService& svc) {
    auto id = pathId(ctx.request);
    if (auto* error = std::get_if<rapidjson::Document>(&id)) {
      return std::move(*error);
    }
    const auto result = svc.retry(std::get<std::string>(id));
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    logger::info("firmware target retry", std::get<std::string>(id) + " attempt=" +
                                              std::to_string(result.getData().attempt) + " by " +
                                              json::getActorSubjectOrDefault(ctx.request));
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      return JsonObj(alloc).set("attempt", result.getData().attempt).take();
    });
  }

  static rapidjson::Document handleOtaSet(const CapabilityContext& ctx, FirmwareService& svc) {
    const auto& req = ctx.request;
    auto id = pathId(req);
    if (auto* error = std::get_if<rapidjson::Document>(&id)) {
      return std::move(*error);
    }
    const auto enabled = json::getBool(req, "enabled");
    if (!enabled) {
      return ResponseHelper::returnErrorDoc("Missing/invalid field: enabled must be a boolean", 400);
    }
    const auto by = json::getActorSubjectOrDefault(req);
    const auto result = svc.setOtaEnabled(std::get<std::string>(id), *enabled, by);
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    logger::info("firmware OTA permission", std::get<std::string>(id) +
                                                (*enabled ? " enabled" : " disabled") + " by " + by);
    return ResponseHelper::returnSuccessDoc();
  }

  // {"state", "otaEnabled", "running": {...} | null, "target": {...} | null}
  static rapidjson::Document handleStatus(const CapabilityContext& ctx, FirmwareService& svc) {
    auto id = pathId(ctx.request);
    if (auto* error = std::get_if<rapidjson::Document>(&id)) {
      return std::move(*error);
    }
    const auto result = [&] {
      auto t = ctx.profiler.scoped("status");
      return svc.status(std::get<std::string>(id));
    }();
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    const auto& status = result.getData();
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      JsonObj obj(alloc);
      obj.set("state", toString(status.state)).set("otaEnabled", status.device.otaEnabled);
      if (status.declared) {
        JsonObj running(alloc);
        running.set("buildId", status.declared->build)
            .set("version", status.declared->version)
            .setOrNull("boardClass", status.declared->boardClass)
            .setOrNull("rejected", status.declared->rejected)
            .setOrNull("percent", status.declared->percent)
            .set("declaredAt", status.declared->declaredAt);
        obj.setValue("running", running.take());
      } else {
        obj.setNull("running");
      }
      if (status.device.target) {
        const auto& t = *status.device.target;
        auto image = imageJson(t.image, alloc);
        JsonObj target(alloc);
        target.setValue("image", std::move(image))
            .set("attempt", t.attempt)
            .set("allowBench", t.allowBench)
            .set("setAt", t.setAt)
            .set("setBy", t.setBy);
        obj.setValue("target", target.take());
      } else {
        obj.setNull("target");
      }
      return obj.take();
    });
  }
};

static AppFirmwareService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string serviceId = "firmware_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "firmware_dev";
    machineName = "dev-machine";
  }

  logger::init("firmware_service", "info", serviceId);

  AppFirmwareService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);
  service.run();
  return 0;
}
