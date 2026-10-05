//
// IngestionService — CoAP/DTLS server receiving sensor readings from devices/
// SensorSimulatorService (Plano_Ingestion.md). Stateless quanto à persistência: não
// conecta no Postgres, só ao gateway (device_credential.list_active, refresh
// periódico do cache de PSKs) e ao RabbitMQ (produtor, uma mensagem por leitura).
//
// Identidade: o device vem da PSK identity da sessão DTLS; um device_id no corpo diferente
// dele é rejeitado com 4.03, e leituras de sensor de outro device (ou desconhecido) são
// descartadas com log (Plano_Telemetria.md, Fase 0). O mapa sensor -> device vem de
// sensor.list_owners, no mesmo refresh periódico das PSKs; enquanto ele nunca carregou,
// a resposta é 5.03 para o device reenviar depois em vez de perder dados.
//
// Formatos (despacho pelo Content-Format da requisição): 110 = SenML JSON
// (Plano_Telemetria.md, D1–D3/DP1–DP3); 50 ou ausente = JSON legado {device_id, readings}.
//
// Validação: só formato mínimo (campos obrigatórios presentes/tipos corretos) —
// validação completa contra device_config fica para uma iteração futura
// (Plano_Ingestion_Implementacao.md).
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/amqp/amqp_client.h"
#include "../../shared/config/config.h"
#include "../../shared/crypto/credential_cipher.h"
#include "../../shared/senml/conversion.h"
#include "../../shared/senml/senml.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <coap3/coap.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <map>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace servicegateway;
namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

class AppIngestionService {
public:
  AppIngestionService(std::string serviceId, std::string machineName, std::string gatewayAddress,
                      std::string coapBindHost, uint16_t coapPort, std::string mqHost,
                      uint16_t mqPort, std::string mqUser, std::string mqPassword)
      : serviceId_(std::move(serviceId)), machineName_(std::move(machineName)),
        gatewayAddress_(std::move(gatewayAddress)), coapBindHost_(std::move(coapBindHost)),
        coapPort_(coapPort),
        producer_(mqHost, mqPort, mqUser, mqPassword, "sensor_readings"),
        telemetryProducer_(std::move(mqHost), mqPort, std::move(mqUser), std::move(mqPassword),
                           "device_telemetry") {}

  bool initialize() {
    if (!producer_.connect() || !telemetryProducer_.connect()) {
      logger::error("IngestionService: failed to connect to RabbitMQ", "");
      return false;
    }

    ServiceIdentity identity;
    identity.serviceName = "ingestion_service";
    identity.serviceId = serviceId_;
    identity.machineName = machineName_;
    identity.version = "v1.0.0";
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 1;
    identity.capabilities = {}; // pure client — never serves a capability
    credentialClient_ = std::make_unique<ServiceClient>(identity, gatewayAddress_);

    return true;
  }

  void run() {
    running_.store(true);
    clientThread_ = std::thread([this] { credentialClient_->run(); });

    // ServiceClient::run() connects/registers on its own thread — wait for that to
    // land before the first credential load, or it silently no-ops (still connecting)
    // and the cache stays empty for a full kRefreshIntervalSec.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!credentialClient_->isConnected() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    refreshCaches(); // synchronous first load, so the caches aren't empty at startup
    refreshThread_ = std::thread([this] { refreshLoop(); });

    runCoapServer(); // blocks until shutdown()

    logger::info("IngestionService stopped", "");
  }

  void shutdown() {
    running_.store(false);
    if (refreshThread_.joinable()) refreshThread_.join();
    if (credentialClient_) credentialClient_->stop();
    if (clientThread_.joinable()) clientThread_.join();
  }

private:
  std::string serviceId_;
  std::string machineName_;
  std::string gatewayAddress_;
  std::string coapBindHost_;
  uint16_t coapPort_;

  rdws::amqp::AmqpProducer producer_;
  rdws::amqp::AmqpProducer telemetryProducer_; // device diagnostics (Plano_Telemetria.md D5)
  std::unique_ptr<ServiceClient> credentialClient_;
  std::thread clientThread_;
  std::thread refreshThread_;
  std::atomic<bool> running_{false};

  // psk_identity -> plaintext key + owning device, refreshed periodically (poll instead of the
  // EventBus bridge that Plano_DeviceCredentials.md envisions — see
  // Plano_Ingestion_Implementacao.md for the trade-off).
  struct CachedCredential {
    std::string key;
    std::string deviceId;
    coap_bin_const_t bin{}; // .s points into `key`'s storage, kept alive by the map
  };
  std::mutex cacheMutex_;
  std::unordered_map<std::string, CachedCredential> pskCache_;

  // sensor_id -> owner + sensors.unit (sensor.list_owners). A failed refresh keeps the
  // previous map.
  struct SensorInfo {
    std::string deviceId;
    std::string unit; // sensors.unit, target of the SenML unit conversion (DP1)
  };
  std::mutex ownersMutex_;
  std::unordered_map<std::string, SensorInfo> sensorOwners_;
  bool ownersLoaded_ = false;

  static constexpr int kRefreshIntervalSec = 60;
  static constexpr unsigned kSessionTimeoutSec = 60;

  void refreshLoop() {
    while (running_.load()) {
      for (int i = 0; i < kRefreshIntervalSec && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (running_.load()) {
        refreshCaches();
      }
    }
  }

  void refreshCaches() {
    if (!credentialClient_ || !credentialClient_->isConnected()) {
      logger::warn("IngestionService: not connected to gateway, skipping cache refresh", "");
      return;
    }
    refreshCredentials();
    refreshSensorOwners();
  }

  void refreshSensorOwners() {
    rapidjson::Document req(rapidjson::kObjectType);
    const auto result = credentialClient_->invoke("sensor.list_owners", req);
    if (!result.success) {
      logger::error("sensor.list_owners failed", result.errorMessage);
      return;
    }

    rapidjson::Document envelope;
    if (envelope.Parse(result.responsePayload.c_str()).HasParseError() || !envelope.IsObject()) {
      return;
    }
    const auto* dataArr = json::getArray(envelope, "data");
    if (dataArr == nullptr) {
      return;
    }

    std::unordered_map<std::string, SensorInfo> owners;
    for (const auto& entry : dataArr->GetArray()) {
      const auto sensorId = json::getString(entry, "id");
      const auto deviceId = json::getString(entry, "device_id");
      if (sensorId && deviceId) {
        owners.emplace(*sensorId,
                       SensorInfo{*deviceId, json::getString(entry, "unit").value_or("")});
      }
    }

    std::scoped_lock lock(ownersMutex_);
    sensorOwners_ = std::move(owners);
    ownersLoaded_ = true;
    logger::info("IngestionService: sensor owner cache refreshed",
                 "count=" + std::to_string(sensorOwners_.size()));
  }

  void refreshCredentials() {

    rapidjson::Document req(rapidjson::kObjectType);
    const auto result = credentialClient_->invoke("device_credential.list_active", req);
    if (!result.success) {
      logger::error("device_credential.list_active failed", result.errorMessage);
      return;
    }

    rapidjson::Document envelope;
    if (envelope.Parse(result.responsePayload.c_str()).HasParseError() || !envelope.IsObject()) {
      return;
    }
    const auto* dataArr = json::getArray(envelope, "data");
    if (dataArr == nullptr) {
      return;
    }

    std::scoped_lock lock(cacheMutex_);
    pskCache_.clear();
    for (const auto& entry : dataArr->GetArray()) {
      const auto identity = json::getString(entry, "psk_identity");
      const auto keyHex = json::getString(entry, "psk_key");
      const auto deviceId = json::getString(entry, "device_id");
      if (!identity || !keyHex || !deviceId) {
        continue;
      }
      const auto keyBytes = rdws::crypto::fromHex(*keyHex);
      CachedCredential cred;
      cred.key = std::string(keyBytes.begin(), keyBytes.end());
      cred.deviceId = *deviceId;
      auto [it, inserted] = pskCache_.emplace(*identity, std::move(cred));
      it->second.bin.length = it->second.key.size();
      it->second.bin.s = reinterpret_cast<const uint8_t*>(it->second.key.data());
    }
    logger::info("IngestionService: credential cache refreshed",
                "count=" + std::to_string(pskCache_.size()));
  }

  static const coap_bin_const_t* pskLookup(coap_bin_const_t* identity, coap_session_t* /*session*/,
                                           void* arg) {
    auto* self = static_cast<AppIngestionService*>(arg);
    std::scoped_lock lock(self->cacheMutex_);
    const std::string identityStr(reinterpret_cast<const char*>(identity->s), identity->length);
    const auto it = self->pskCache_.find(identityStr);
    if (it == self->pskCache_.end()) {
      return nullptr;
    }
    return &it->second.bin;
  }

  void runCoapServer() {
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] { coap_startup(); });

    coap_context_t* ctx = coap_new_context(nullptr);
    if (!ctx) {
      logger::error("IngestionService: coap_new_context failed", "");
      return;
    }
    coap_context_set_app_data(ctx, this);
    // Mirror the client's block-mode (coap_dtls_client.cpp) — SINGLE_BODY makes
    // libcoap reassemble a fragmented (Block1) request before calling our resource
    // handler, so onRequest() always sees the complete payload in one shot.
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);
    // Idle sessions go after 60 s instead of libcoap's 300 s. The eswifi module reconnects
    // from the same source port (5684) after a reboot, so a station reset mid-cycle (no
    // close_notify) has its new ClientHello delivered to the stale session and ignored; its
    // ~31 s of retries keep that session alive. With a short timeout the stale session
    // expires between uplink cycles and the next cycle handshakes normally.
    coap_context_set_session_timeout(ctx, kSessionTimeoutSec);

    coap_dtls_spsk_t setupData{};
    setupData.version = COAP_DTLS_SPSK_SETUP_VERSION;
    setupData.validate_id_call_back = pskLookup;
    setupData.id_call_back_arg = this;
    coap_context_set_psk2(ctx, &setupData);

    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port = htons(coapPort_);
    addr.addr.sin.sin_addr.s_addr =
        coapBindHost_ == "0.0.0.0" ? htonl(INADDR_ANY) : inet_addr(coapBindHost_.c_str());
    coap_new_endpoint(ctx, &addr, COAP_PROTO_DTLS);

    coap_str_const_t rootUri{0, reinterpret_cast<const uint8_t*>("")};
    coap_resource_t* resource = coap_resource_init(&rootUri, 0);
    coap_register_request_handler(resource, COAP_REQUEST_POST,
                                  [](coap_resource_t* res, coap_session_t* session,
                                     const coap_pdu_t* request, const coap_string_t* query,
                                     coap_pdu_t* response) {
                                    auto* self = static_cast<AppIngestionService*>(
                                        coap_context_get_app_data(coap_session_get_context(session)));
                                    self->onRequest(session, request, response);
                                  });
    coap_add_resource(ctx, resource);

    logger::info("IngestionService listening", "coap://" + coapBindHost_ + ":" +
                                                  std::to_string(coapPort_) + " (DTLS)");
    while (running_.load()) {
      coap_io_process(ctx, 1000);
    }

    coap_free_context(ctx);
  }

  // Device bound to the DTLS session's PSK identity (Plano_Telemetria.md, Fase 0). Empty if
  // the credential left the cache (revoked/rotated) after the handshake.
  std::string authenticatedDeviceId(coap_session_t* session) {
    const coap_bin_const_t* identity = coap_session_get_psk_identity(session);
    if (identity == nullptr) {
      return {};
    }
    const std::string identityStr(reinterpret_cast<const char*>(identity->s), identity->length);
    std::scoped_lock lock(cacheMutex_);
    const auto it = pskCache_.find(identityStr);
    return it == pskCache_.end() ? std::string{} : it->second.deviceId;
  }

  void onRequest(coap_session_t* session, const coap_pdu_t* request, coap_pdu_t* response) {
    const auto deviceId = authenticatedDeviceId(session);
    if (deviceId.empty()) {
      logger::warn("IngestionService: session has no known PSK identity, rejecting", "");
      coap_pdu_set_code(response, COAP_RESPONSE_CODE_UNAUTHORIZED);
      return;
    }

    const uint8_t* data = nullptr;
    size_t len = 0, offset = 0, total = 0;
    coap_get_data_large(request, &len, &data, &offset, &total);

    const auto body = (data != nullptr && len > 0)
        ? std::string(reinterpret_cast<const char*>(data), len)
        : std::string{};

    const int result = isSenml(request) ? handleSenml(deviceId, body)
                                        : handlePayload(deviceId, body);

    coap_pdu_set_code(response, result == kForbidden     ? COAP_RESPONSE_CODE_FORBIDDEN
                                : result == kFormatError ? COAP_RESPONSE_CODE_BAD_REQUEST
                                : result == kUnavailable ? COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE
                                                         : COAP_RESPONSE_CODE_CHANGED);
  }

  // Optional `location: {"lat": ..., "lon": ...}` on the payload (Plano_Ingestion.md) -
  // gateways without a fixed installation point (e.g. a mobile/roaming device) report
  // their position on every send cycle. Fire-and-forget over the same broker connection
  // already used for credential refresh; a failure here shouldn't block reading ingestion.
  void reportLocation(const std::string& deviceId, const rapidjson::Value& location) {
    const auto lat = json::getDouble(location, "lat");
    const auto lon = json::getDouble(location, "lon");
    if (!lat || !lon) {
      logger::warn("IngestionService: location present but missing lat/lon", "");
      return;
    }

    const std::string wkt = "POINT(" + std::to_string(*lon) + " " + std::to_string(*lat) + ")";

    rapidjson::Document req(rapidjson::kObjectType);
    auto& alloc = req.GetAllocator();
    req.AddMember("device_id", rapidjson::Value(deviceId.c_str(), alloc), alloc);
    req.AddMember("location", rapidjson::Value(wkt.c_str(), alloc), alloc);

    const auto result = credentialClient_->invoke("device.update_location", req);
    if (!result.success) {
      logger::warn("IngestionService: device.update_location failed", result.errorMessage);
    }
  }

  static constexpr int kFormatError = -1;
  static constexpr int kForbidden = -2;
  static constexpr int kUnavailable = -3;

  static constexpr unsigned kContentFormatSenmlJson = 110; // application/senml+json

  static bool isSenml(const coap_pdu_t* request) {
    coap_opt_iterator_t it;
    const coap_opt_t* opt = coap_check_option(request, COAP_OPTION_CONTENT_FORMAT, &it);
    return opt != nullptr && coap_decode_var_bytes(coap_opt_value(opt), coap_opt_length(opt)) ==
                                 kContentFormatSenmlJson;
  }

  // Snapshot under the lock: publishing talks to RabbitMQ and shouldn't hold it. nullopt
  // while the cache has never loaded (caller answers kUnavailable).
  std::optional<std::unordered_map<std::string, SensorInfo>> snapshotOwners() {
    std::scoped_lock lock(ownersMutex_);
    if (!ownersLoaded_) {
      logger::warn("IngestionService: sensor owner cache not loaded yet, asking to retry", "");
      return std::nullopt;
    }
    return sensorOwners_;
  }

  // False (and logs) when the sensor isn't registered to the authenticated device.
  static bool ownedBy(const std::unordered_map<std::string, SensorInfo>& owners,
                      const std::string& sensorId, const std::string& deviceId) {
    const auto owner = owners.find(sensorId);
    if (owner != owners.end() && owner->second.deviceId == deviceId) {
      return true;
    }
    logger::warn("IngestionService: sensor doesn't belong to the device, dropping reading",
                 "device_id=" + deviceId + " sensor_id=" + sensorId + " owner=" +
                     (owner == owners.end() ? std::string("unknown") : owner->second.deviceId));
    return false;
  }

  bool publishReading(const std::string& deviceId, const std::string& sensorId,
                      const std::string& timestamp, double value, const std::string& unit,
                      int flags) {
    rapidjson::Document msg(rapidjson::kObjectType);
    auto& alloc = msg.GetAllocator();
    msg.AddMember("device_id", rapidjson::Value(deviceId.c_str(), alloc), alloc);
    msg.AddMember("sensor_id", rapidjson::Value(sensorId.c_str(), alloc), alloc);
    msg.AddMember("timestamp", rapidjson::Value(timestamp.c_str(), alloc), alloc);
    msg.AddMember("value", value, alloc);
    msg.AddMember("unit", rapidjson::Value(unit.c_str(), alloc), alloc);
    msg.AddMember("flags", flags, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    msg.Accept(writer);

    if (!producer_.publish(buffer.GetString())) {
      logger::error("IngestionService: failed to publish reading to RabbitMQ", "");
      return false;
    }
    return true;
  }

  // One device_telemetry row per instant: {"device_id", "timestamp", "data": {name: value}}.
  // A failed publish only logs — diagnostics never block the readings' 2.04.
  void publishTelemetry(const std::string& deviceId, double time,
                        const std::vector<const rdws::senml::Record*>& records) {
    rapidjson::Document msg(rapidjson::kObjectType);
    auto& alloc = msg.GetAllocator();
    rapidjson::Value data(rapidjson::kObjectType);
    const size_t prefixLen = deviceId.size() + 1; // "<device_id>/"
    for (const auto* rec : records) {
      rapidjson::Value key(rec->name.substr(prefixLen).c_str(), alloc);
      rapidjson::Value value;
      if (rec->value) {
        // Counters (seq, boot_count, ...) stay integers in the JSONB instead of 812.0.
        const double v = *rec->value;
        if (std::trunc(v) == v && std::fabs(v) < 9.0e15) {
          value.SetInt64(static_cast<int64_t>(v));
        } else {
          value.SetDouble(v);
        }
      } else if (rec->boolValue) {
        value.SetBool(*rec->boolValue);
      } else {
        value.SetString(rec->stringValue->c_str(), alloc);
      }
      data.RemoveMember(key); // a repeated name at the same instant: last one wins
      data.AddMember(key, value, alloc);
    }
    msg.AddMember("device_id", rapidjson::Value(deviceId.c_str(), alloc), alloc);
    msg.AddMember("timestamp", rapidjson::Value(rdws::senml::toIso8601(time).c_str(), alloc),
                  alloc);
    msg.AddMember("data", data, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    msg.Accept(writer);
    if (!telemetryProducer_.publish(buffer.GetString())) {
      logger::error("IngestionService: failed to publish device telemetry to RabbitMQ", "");
    }
  }

  // SenML JSON pack (Plano_Telemetria.md). Names are bn + n with bn = "<device_id>/" (DP2):
  // a numeric remainder is a global sensor_id (F1); anything else ("seq" included) is device
  // diagnostics (D2), grouped by instant into device_telemetry rows (D5).
  // Whole pack: unparseable -> kFormatError, any name outside the authenticated device ->
  // kForbidden. Per record (dropped with a log, the rest goes on): foreign sensor, no
  // numeric value, time_unsynced or unknown fl_ bits (DP3/F3), unit without a rule (DP1).
  int handleSenml(const std::string& authDeviceId, const std::string& body) {
    const auto now = static_cast<double>(std::time(nullptr));
    const auto pack = rdws::senml::parse(body, now);
    if (!pack) {
      logger::warn("IngestionService: malformed SenML pack", pack.error());
      return kFormatError;
    }

    const std::string prefix = authDeviceId + "/";
    for (const auto& rec : *pack) {
      if (rec.name.compare(0, prefix.size(), prefix) != 0) {
        logger::warn("IngestionService: SenML name outside the PSK's device, rejecting",
                     "authenticated=" + authDeviceId + " name=" + rec.name);
        return kForbidden;
      }
    }

    const auto owners = snapshotOwners();
    if (!owners) {
      return kUnavailable;
    }

    int published = 0;
    int sensorRecords = 0;
    std::string seq = "-";
    std::string diagnostics;
    std::map<double, std::vector<const rdws::senml::Record*>> telemetry;
    for (const auto& rec : *pack) {
      const std::string local = rec.name.substr(prefix.size());
      const bool numeric = !local.empty() && std::all_of(local.begin(), local.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      });

      if (!numeric) {
        if (local == "seq") {
          seq = rec.value ? std::to_string(static_cast<long long>(*rec.value)) : "?";
        } else {
          diagnostics += (diagnostics.empty() ? "" : ",") + local;
        }
        telemetry[rec.time].push_back(&rec);
        continue;
      }

      ++sensorRecords;
      const std::string ctx = "device_id=" + authDeviceId + " sensor_id=" + local;
      if (!ownedBy(*owners, local, authDeviceId)) {
        continue;
      }
      if (!rec.value) {
        logger::warn("IngestionService: SenML sensor record without numeric v, dropping", ctx);
        continue;
      }
      if ((rec.flags & rdws::senml::kFlagTimeUnsynced) != 0 ||
          (rec.flags & ~rdws::senml::kKnownFlags) != 0) {
        logger::warn("IngestionService: SenML record unsynced or with unknown flags, dropping",
                     ctx + " fl_=" + std::to_string(rec.flags));
        continue;
      }
      const auto& sensorUnit = owners->at(local).unit;
      const auto value = rdws::senml::toSensorUnit(*rec.value, rec.unit, sensorUnit);
      if (!value) {
        logger::warn("IngestionService: SenML unit has no conversion, dropping",
                     ctx + " u=" + rec.unit + " sensor_unit=" + sensorUnit);
        continue;
      }

      if (publishReading(authDeviceId, local, rdws::senml::toIso8601(rec.time), *value,
                         sensorUnit, rec.flags)) {
        ++published;
      }
    }

    for (const auto& [time, records] : telemetry) {
      publishTelemetry(authDeviceId, time, records);
    }

    logger::info("IngestionService: SenML pack",
                 "device_id=" + authDeviceId + " seq=" + seq + " records=" +
                     std::to_string(pack->size()) + " sensor_records=" +
                     std::to_string(sensorRecords) + " published=" + std::to_string(published) +
                     " diagnostics=" + (diagnostics.empty() ? "-" : diagnostics));
    return published;
  }

  // Returns the number of readings published, kFormatError on a malformed payload (missing
  // device_id/readings), kForbidden when the body's device_id isn't the authenticated one, or
  // kUnavailable while the sensor owner cache has never loaded.
  int handlePayload(const std::string& authDeviceId, const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      logger::warn("IngestionService: malformed JSON payload", "");
      return kFormatError;
    }
    const auto deviceId = json::getString(doc, "device_id");
    const auto* readings = json::getArray(doc, "readings");
    if (!deviceId || readings == nullptr) {
      logger::warn("IngestionService: payload missing device_id/readings", "");
      return kFormatError;
    }
    if (*deviceId != authDeviceId) {
      logger::warn("IngestionService: payload device_id doesn't match the PSK, rejecting",
                   "authenticated=" + authDeviceId + " payload=" + *deviceId);
      return kForbidden;
    }

    const auto owners = snapshotOwners();
    if (!owners) {
      return kUnavailable;
    }

    if (const auto* location = json::getObject(doc, "location")) {
      reportLocation(*deviceId, *location);
    }

    int published = 0;
    for (const auto& reading : readings->GetArray()) {
      const auto sensorId = json::getString(reading, "sensor_id");
      const auto timestamp = json::getString(reading, "timestamp");
      const auto value = json::getDouble(reading, "value");
      const auto unit = json::getString(reading, "unit");
      if (!sensorId || !timestamp || !value) {
        logger::warn("IngestionService: reading missing required field, skipping", "");
        continue;
      }
      if (!ownedBy(*owners, *sensorId, authDeviceId)) {
        continue;
      }
      if (publishReading(*deviceId, *sensorId, *timestamp, *value, unit.value_or(""), 0)) {
        ++published;
      }
    }
    return published;
  }
};

static AppIngestionService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string serviceId = "ingestion_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "ingestion_dev";
    machineName = "dev-machine";
  }

  logger::init("ingestion_service", "info", serviceId);
  rdws::Config(); // loads .env for native/dev runs before plain getenv() below

  const std::string coapBindHost = rdws::Config::getEnvVarOrDefault("INGESTION_BIND_HOST", "0.0.0.0");
  const uint16_t coapPort =
      static_cast<uint16_t>(std::stoi(rdws::Config::getEnvVarOrDefault("INGESTION_COAP_PORT", "5684")));
  const std::string mqHost = rdws::Config::getEnvVarOrDefault("RABBITMQ_HOST", "localhost");
  const uint16_t mqPort = static_cast<uint16_t>(std::stoi(rdws::Config::getEnvVarOrDefault("RABBITMQ_PORT", "5672")));
  const std::string mqUser = rdws::Config::getEnvVarOrDefault("RABBITMQ_USER", "guest");
  const std::string mqPassword = rdws::Config::getEnvVarOrDefault("RABBITMQ_PASSWORD", "guest");

  AppIngestionService service(serviceId, machineName, gatewayAddress, coapBindHost, coapPort,
                              mqHost, mqPort, mqUser, mqPassword);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize IngestionService", "");
    return 1;
  }
  service.run();
  return 0;
}
