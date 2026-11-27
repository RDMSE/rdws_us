// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: AGPL-3.0-only

//
// FirmwareService — signed firmware images and which one each device should run (OTA,
// docs/Plano_OTA.md). Capabilities, all admin only over HTTP:
//   firmware_image.create / list / withdraw
//   firmware_target.set / retry, firmware_ota.set, firmware.status
// and firmware.offer, internal only (no route): asked by the ingestion with what a station declared,
// it says which image to offer in the 2.04 (O3).
// Path parameter {id} is a public id (UUID): the image's or the device's. No route reaches more than
// one device (O17).
//
// The image files come from a read-only volume (FIRMWARE_IMAGES_DIR), filled by the publish script.
// FirmwareCoapServer delivers them to the stations (GET /fw, CoAP/DTLS), in a thread of its own, with
// its own database connection; the stations' PSKs come from device_service, through the gateway.

#include "FirmwareCoapServer.h"

#include "../../shared/broker/ServiceClient.h"
#include "../../shared/coap/psk_cache.h"
#include "../../shared/config/config.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/firmware/file_image_store.h"
#include "../../shared/firmware/image_delivery.h"
#include "../../shared/firmware/target_delivery.h"
#include "../../shared/repository/FirmwareRepository.h"
#include "../../shared/service/FirmwareService.h"
#include "../../shared/service/client/DeviceDiagnosticsClient.h"
#include "../../shared/utils/authorization.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/lambda_params_helper.h"
#include "../../shared/utils/logger.h"
#include "../../shared/utils/profiler.h"
#include "../../shared/utils/refresh_schedule.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/validation.h"
#include "rdws_version.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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

std::string coapHost() {
  const char* host = std::getenv("FIRMWARE_BIND_HOST"); // NOLINT(concurrency-mt-unsafe)
  return host != nullptr && *host != '\0' ? host : "0.0.0.0";
}

// `fallback` when it is unset, isn't a whole number or is out of range
unsigned envNumber(const char* name, unsigned fallback, unsigned max) {
  const char* text = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
  if (text == nullptr) {
    return fallback;
  }
  unsigned value = 0;
  const char* end = text + std::strlen(text);
  const auto [stop, error] = std::from_chars(text, end, value);
  if (error != std::errc{} || stop != end || value == 0 || value > max) {
    logger::warn("Invalid setting, using the default", name);
    return fallback;
  }
  return value;
}

constexpr unsigned kDefaultCoapPort = 5690;
constexpr unsigned kDefaultMaxDownloads = 4;
constexpr int kRefreshIntervalSec = 60;
constexpr int kFirstLoadRetrySec = 5;

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

  // Delivery: used by the CoAP thread only, but the credentials
  rdws::database::PostgreSQLDatabase deliveryDb_;
  FirmwareRepository deliveryRepo_;
  TargetDelivery targets_;
  ImageDelivery delivery_;
  rdws::coap::PskCache credentials_;
  FirmwareCoapServer coap_;
  std::mutex clientMutex_; // connect() replaces `client` while the refresh may be using it
  std::atomic<bool> failed_{false};
  unsigned offerPort_; // what the stations reach: on QA, through a relay, not the same

public:
  AppFirmwareService(const std::string& serviceId, const std::string& machineName,
                     std::string broker)
      : gatewayAddress(std::move(broker)), repo_(db_), store_(imagesDirectory()),
        declaredSource_(client), svc_(repo_, store_, declaredSource_), deliveryRepo_(deliveryDb_),
        targets_(deliveryRepo_),
        delivery_(targets_, store_,
                  envNumber("FIRMWARE_MAX_DOWNLOADS", kDefaultMaxDownloads, 1000)),
        coap_(credentials_, delivery_, coapHost(),
              static_cast<std::uint16_t>(
                  envNumber("FIRMWARE_COAP_PORT", kDefaultCoapPort, UINT16_MAX))),
        offerPort_(envNumber("FIRMWARE_OFFER_PORT",
                             envNumber("FIRMWARE_COAP_PORT", kDefaultCoapPort, UINT16_MAX),
                             UINT16_MAX)) {
    identity.machineName = machineName;
    identity.serviceName = "firmware_service";
    identity.serviceId = serviceId;
    identity.version = rdws::version();
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 20;
    identity.capabilities = {"firmware_image.create", "firmware_image.list",
                             "firmware_image.withdraw", "firmware_target.set",
                             "firmware_target.retry",  "firmware_ota.set",
                             "firmware.status",        "firmware.offer"};
  }

  // False when the delivery couldn't start
  bool run() {
    running.store(true);
    logger::info("FirmwareService starting", identity.serviceId + " images=" + imagesDirectory());
    connect();
    std::thread refresh([this] { refreshLoop(); });
    std::thread delivery([this] {
      if (!coap_.serve(running)) {
        logger::error("Firmware delivery can't open its port", identity.serviceId);
        failed_.store(true);
        shutdown();
      }
    });
    for (bool first = true; running.load(); first = false) {
      if (!first) {
        connect();
      }
      client->run();
      if (!running.load()) {
        break;
      }
      logger::warn("Reconnecting in 3s", identity.serviceId);
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    refresh.join();
    delivery.join();
    logger::info("FirmwareService stopped", identity.serviceId);
    return !failed_.load();
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
  }

private:
  void connect() {
    auto next = std::make_unique<ServiceClient>(identity, gatewayAddress);
    next->setRequestHandler(
        [this](const rapidjson::Document& req) { return processRequest(req); });
    std::scoped_lock lock(clientMutex_);
    client = std::move(next);
  }

  void refreshLoop() {
    for (int wait = 0; running.load(); --wait) {
      if (wait <= 0) {
        refreshCredentials();
        wait = rdws::utils::nextRefreshDelaySec(credentials_.loaded(), kRefreshIntervalSec,
                                                kFirstLoadRetrySec);
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  void refreshCredentials() {
    InvokeResult result;
    {
      std::scoped_lock lock(clientMutex_);
      if (!client->isConnected()) {
        return;
      }
      result = client->invoke("device_credential.list_active",
                              rapidjson::Document(rapidjson::kObjectType));
    }
    if (!result.success) {
      logger::error("device_credential.list_active failed", result.errorMessage);
      return;
    }
    const bool first = !credentials_.loaded();
    if (!credentials_.load(result.responsePayload)) {
      logger::error("device_credential.list_active: unexpected answer, keeping the cache", "");
      return;
    }
    if (credentials_.skipped() > 0) {
      logger::error("device_credential.list_active: invalid credentials left out",
                    "count=" + std::to_string(credentials_.skipped()));
    }
    if (first) {
      logger::info("Firmware delivery: credentials loaded",
                   "count=" + std::to_string(credentials_.size()));
    }
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
            {"firmware.offer", [this](const CapabilityContext& ctx, FirmwareService& svc) {
               return handleOffer(ctx, svc, offerPort_);
             }},
        };

    // Before any handler: none of them is for anyone else. The offer is for the ingestion only.
    auto denied = cap == "firmware.offer" ? rdws::utils::requireInternal(request)
                                          : rdws::utils::requireAdmin(request);
    if (denied) {
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

  // {"device": "<public id>", "fw_build", "fw_class"?, "fw_rej"?} → {"offer": {...} | null,
  // "reason"}. The caller decides whether the station asked (`ota`) and whether a config goes first.
  static rapidjson::Document handleOffer(const CapabilityContext& ctx, FirmwareService& svc,
                                         unsigned port) {
    const auto& req = ctx.request;
    const auto device = json::getString(req, "device").value_or("");
    const auto build = json::getString(req, "fw_build");
    if (!isUuid(device) || !build) {
      return ResponseHelper::returnErrorDoc("Missing/invalid field: device, fw_build", 400);
    }
    const auto result = svc.offer(device, {.build = *build,
                                           .boardClass = json::getString(req, "fw_class"),
                                           .rejected = json::getString(req, "fw_rej")});
    if (!result.isSuccess()) {
      return errorOf(result);
    }
    const auto& decision = result.getData();
    return ResponseHelper::returnDataDoc([&](auto& alloc) {
      JsonObj obj(alloc);
      if (decision.offer) {
        const auto& o = *decision.offer;
        JsonObj offer(alloc);
        offer.set("build", o.build)
            .set("size", o.size)
            .set("sha256", o.sha256)
            .set("class", o.boardClass)
            .set("port", static_cast<int>(port))
            .set("try", o.attempt);
        obj.setValue("offer", offer.take()).setNull("reason");
      } else {
        obj.setNull("offer").set("reason", toString(decision.reason));
      }
      return obj.take();
    });
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
  return service.run() ? 0 : 1;
}
