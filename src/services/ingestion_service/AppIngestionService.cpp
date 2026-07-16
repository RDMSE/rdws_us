//
// IngestionService — CoAP/DTLS server receiving sensor readings from devices/
// SensorSimulatorService (Plano_Ingestion.md). Stateless quanto à persistência: não
// conecta no Postgres, só ao gateway (device_credential.list_active, refresh
// periódico do cache de PSKs) e ao RabbitMQ (produtor, uma mensagem por leitura).
//
// Validação: só formato mínimo (campos obrigatórios presentes/tipos corretos) —
// validação completa contra device_config fica para uma iteração futura
// (Plano_Ingestion_Implementacao.md).
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/amqp/amqp_client.h"
#include "../../shared/config/config.h"
#include "../../shared/crypto/credential_cipher.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <coap3/coap.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

using namespace servicegateway;
namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace {

std::string getenvOrDefault(const char* name, const std::string& def) {
  const char* value = std::getenv(name);
  return (value != nullptr && std::string(value).length() > 0) ? value : def;
}

} // namespace

class AppIngestionService {
public:
  AppIngestionService(std::string serviceId, std::string machineName, std::string gatewayAddress,
                      std::string coapBindHost, uint16_t coapPort, std::string mqHost,
                      uint16_t mqPort, std::string mqUser, std::string mqPassword)
      : serviceId_(std::move(serviceId)), machineName_(std::move(machineName)),
        gatewayAddress_(std::move(gatewayAddress)), coapBindHost_(std::move(coapBindHost)),
        coapPort_(coapPort),
        producer_(std::move(mqHost), mqPort, std::move(mqUser), std::move(mqPassword),
                 "sensor_readings") {}

  bool initialize() {
    if (!producer_.connect()) {
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
    refreshCache(); // synchronous first load, so the cache isn't empty at startup
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
  std::unique_ptr<ServiceClient> credentialClient_;
  std::thread clientThread_;
  std::thread refreshThread_;
  std::atomic<bool> running_{false};

  // psk_identity -> plaintext key, refreshed periodically (poll instead of the
  // EventBus bridge that Plano_DeviceCredentials.md envisions — see
  // Plano_Ingestion_Implementacao.md for the trade-off).
  struct CachedCredential {
    std::string key;
    coap_bin_const_t bin{}; // .s points into `key`'s storage, kept alive by the map
  };
  std::mutex cacheMutex_;
  std::unordered_map<std::string, CachedCredential> pskCache_;

  static constexpr int kRefreshIntervalSec = 60;

  void refreshLoop() {
    while (running_.load()) {
      for (int i = 0; i < kRefreshIntervalSec && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (running_.load()) {
        refreshCache();
      }
    }
  }

  void refreshCache() {
    if (!credentialClient_ || !credentialClient_->isConnected()) {
      logger::warn("IngestionService: not connected to gateway, skipping credential refresh", "");
      return;
    }

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
      if (!identity || !keyHex) {
        continue;
      }
      const auto keyBytes = rdws::crypto::fromHex(*keyHex);
      CachedCredential cred;
      cred.key = std::string(keyBytes.begin(), keyBytes.end());
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
                                    self->onRequest(request, response);
                                  });
    coap_add_resource(ctx, resource);

    logger::info("IngestionService listening", "coap://" + coapBindHost_ + ":" +
                                                  std::to_string(coapPort_) + " (DTLS)");
    while (running_.load()) {
      coap_io_process(ctx, 1000);
    }

    coap_free_context(ctx);
  }

  void onRequest(const coap_pdu_t* request, coap_pdu_t* response) {
    const uint8_t* data = nullptr;
    size_t len = 0, offset = 0, total = 0;
    coap_get_data_large(request, &len, &data, &offset, &total);

    const std::string body(reinterpret_cast<const char*>(data), len);
    const int publishedCount = handlePayload(body);

    coap_pdu_set_code(response, publishedCount >= 0 ? COAP_RESPONSE_CODE_CHANGED
                                                    : COAP_RESPONSE_CODE_BAD_REQUEST);
  }

  // Returns the number of readings published, or -1 on a format error (missing
  // device_id/readings, or a reading missing required fields).
  int handlePayload(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      logger::warn("IngestionService: malformed JSON payload", "");
      return -1;
    }
    const auto deviceId = json::getString(doc, "device_id");
    const auto* readings = json::getArray(doc, "readings");
    if (!deviceId || readings == nullptr) {
      logger::warn("IngestionService: payload missing device_id/readings", "");
      return -1;
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

      rapidjson::Document msg(rapidjson::kObjectType);
      auto& alloc = msg.GetAllocator();
      msg.AddMember("device_id", rapidjson::Value(deviceId->c_str(), alloc), alloc);
      msg.AddMember("sensor_id", rapidjson::Value(sensorId->c_str(), alloc), alloc);
      msg.AddMember("timestamp", rapidjson::Value(timestamp->c_str(), alloc), alloc);
      msg.AddMember("value", *value, alloc);
      msg.AddMember("unit", rapidjson::Value(unit.value_or("").c_str(), alloc), alloc);

      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      msg.Accept(writer);

      if (producer_.publish(buffer.GetString())) {
        ++published;
      } else {
        logger::error("IngestionService: failed to publish reading to RabbitMQ", "");
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

  const std::string coapBindHost = getenvOrDefault("INGESTION_BIND_HOST", "0.0.0.0");
  const uint16_t coapPort =
      static_cast<uint16_t>(std::stoi(getenvOrDefault("INGESTION_COAP_PORT", "5684")));
  const std::string mqHost = getenvOrDefault("RABBITMQ_HOST", "localhost");
  const uint16_t mqPort = static_cast<uint16_t>(std::stoi(getenvOrDefault("RABBITMQ_PORT", "5672")));
  const std::string mqUser = getenvOrDefault("RABBITMQ_USER", "guest");
  const std::string mqPassword = getenvOrDefault("RABBITMQ_PASSWORD", "guest");

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
