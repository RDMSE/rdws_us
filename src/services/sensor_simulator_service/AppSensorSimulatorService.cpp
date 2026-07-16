//
// SensorSimulatorService — standalone tool (Plano_SensorSimulatorService.md), not a
// gateway capability provider. Direct DB connection for reading its own device/sensor
// config (like the other services), plus a ServiceClient used only as a client (empty
// capability list) to call the internal device_credential.get_active capability before
// each transmission cycle (Plano_DeviceCredentials.md §5 — no cache, fresh credential
// per cycle).
//
// Scope restricted to a single device at startup (--device-id), matching the "one
// instance per simulated device" scheduling model.
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/config/config.h"
#include "../../shared/coap/coap_dtls_client.h"
#include "../../shared/crypto/credential_cipher.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceConfigRepository.h"
#include "../../shared/repository/DeviceRepository.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace servicegateway;
using namespace rdws::database;
namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;

namespace {

// MVP plausible-value ranges by sensor type — the fine-grained internal-sampling-vs-
// transmission-rate design for volatile sensors (wind avg/gust/direction, see
// Plano_Ingestion.md "Amostragem interna vs. taxa de transmissão") is intentionally not
// implemented here yet; this generates one plausible value per sensor per tick.
struct Range {
  double min;
  double max;
};

const std::unordered_map<std::string, Range> kRangesByType = {
    {"temperature", {-5.0, 40.0}},
    {"moisture", {0.0, 100.0}},
    {"ph", {3.0, 9.0}},
    {"humidity", {0.0, 100.0}},
    {"luminosity", {0.0, 100000.0}},
    {"other", {0.0, 100.0}},
};

std::string nowIso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string dataFilePath(const std::string& dataDir, const std::string& deviceId,
                         const std::string& sensorId) {
  return dataDir + "/" + deviceId + "_" + sensorId + ".data";
}

int readTransmissionsPerDay(const std::string& configJson) {
  if (configJson.empty()) {
    return 4;
  }
  rapidjson::Document doc;
  if (doc.Parse(configJson.c_str()).HasParseError() || !doc.IsObject()) {
    return 4;
  }
  return json::getInt(doc, "transmissions_per_day").value_or(4);
}

std::string getenvOrDefault(const char* name, const std::string& def) {
  const char* value = std::getenv(name);
  return (value != nullptr && std::string(value).length() > 0) ? value : def;
}

} // namespace

class AppSensorSimulatorService {
public:
  AppSensorSimulatorService(std::string deviceId, std::string gatewayAddress,
                            std::string ingestionHost, uint16_t ingestionPort,
                            std::string dataDir, int httpControlPort)
      : deviceId_(std::move(deviceId)), gatewayAddress_(std::move(gatewayAddress)),
        ingestionHost_(std::move(ingestionHost)), ingestionPort_(ingestionPort),
        dataDir_(std::move(dataDir)), httpControlPort_(httpControlPort), deviceRepo_(db_),
        deviceConfigRepo_(db_) {}

  bool initialize() {
    db_.connect();

    const auto sensors = deviceRepo_.findSimulatedSensorsByDeviceId(deviceId_);
    if (sensors.empty()) {
      logger::error("No simulated sensors found for device",
                    "device_id=" + deviceId_ +
                        " (device must exist with is_simulated=true and have sensors)");
      return false;
    }
    sensors_ = sensors;

    std::filesystem::create_directories(dataDir_);

    // Client-only ServiceClient: empty capability list — never serves any capability,
    // just calls device_credential.get_active through the gateway before each cycle.
    ServiceIdentity identity;
    identity.serviceName = "sensor_simulator_service";
    identity.serviceId = "simulator_" + deviceId_;
    identity.machineName = "localhost";
    identity.version = "v1.0.0";
    identity.environment = rdws::Config().getEnvironment();
    identity.maxConcurrent = 1;
    identity.capabilities = {};
    credentialClient_ = std::make_unique<ServiceClient>(identity, gatewayAddress_);

    return true;
  }

  void run() {
    running_.store(true);

    clientThread_ = std::thread([this] { credentialClient_->run(); });

    generationThread_ = std::thread([this] { generationLoop(); });
    transmissionThread_ = std::thread([this] { transmissionLoop(); });

    setupHttpControl();
    logger::info("SensorSimulatorService listening",
                "device_id=" + deviceId_ + " control_port=" + std::to_string(httpControlPort_));
    httpServer_.listen("0.0.0.0", httpControlPort_);

    logger::info("SensorSimulatorService stopped", "device_id=" + deviceId_);
  }

  void shutdown() {
    running_.store(false);
    httpServer_.stop();
    if (generationThread_.joinable()) generationThread_.join();
    if (transmissionThread_.joinable()) transmissionThread_.join();
    if (credentialClient_) credentialClient_->stop();
    if (clientThread_.joinable()) clientThread_.join();
  }

private:
  std::string deviceId_;
  std::string gatewayAddress_;
  std::string ingestionHost_;
  uint16_t ingestionPort_;
  std::string dataDir_;
  int httpControlPort_;

  PostgreSQLDatabase db_;
  rdws::device::DeviceRepository deviceRepo_;
  rdws::device_config::DeviceConfigRepository deviceConfigRepo_;
  std::vector<rdws::device::SimulatedSensor> sensors_;

  std::unique_ptr<ServiceClient> credentialClient_;
  std::thread clientThread_;
  std::thread generationThread_;
  std::thread transmissionThread_;
  httplib::Server httpServer_;

  std::atomic<bool> running_{false};
  std::mutex deviceFileMutex_; // guards all <device_id>_<sensor_id>.data files for this device

  static constexpr int kGenerationIntervalSec = 30;

  // ─── Generation ──────────────────────────────────────────────────────────

  void generationLoop() {
    std::mt19937 rng(std::random_device{}());
    while (running_.load()) {
      {
        std::scoped_lock lock(deviceFileMutex_);
        for (const auto& sensor : sensors_) {
          appendReading(sensor, rng);
        }
      }
      for (int i = 0; i < kGenerationIntervalSec && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }

  void appendReading(const rdws::device::SimulatedSensor& sensor, std::mt19937& rng) {
    const auto it = kRangesByType.find(sensor.sensorType);
    const Range range = it != kRangesByType.end() ? it->second : Range{0.0, 100.0};
    std::uniform_real_distribution<double> dist(range.min, range.max);
    const double value = dist(rng);

    const std::string path = dataFilePath(dataDir_, deviceId_, sensor.sensorId);
    rapidjson::Document doc(rapidjson::kArrayType);
    loadJsonArray(path, doc);

    rapidjson::Value reading(rapidjson::kObjectType);
    auto& alloc = doc.GetAllocator();
    reading.AddMember("timestamp", rapidjson::Value(nowIso8601().c_str(), alloc), alloc);
    reading.AddMember("value", value, alloc);
    doc.PushBack(reading, alloc);

    writeJsonArray(path, doc);
  }

  static void loadJsonArray(const std::string& path, rapidjson::Document& doc) {
    std::ifstream in(path);
    if (!in) {
      doc.SetArray();
      return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    if (content.empty() || doc.Parse(content.c_str()).HasParseError() || !doc.IsArray()) {
      doc.SetArray();
    }
  }

  static void writeJsonArray(const std::string& path, const rapidjson::Document& doc) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    std::ofstream out(path, std::ios::trunc);
    out << buffer.GetString();
  }

  // ─── Transmission ────────────────────────────────────────────────────────

  void transmissionLoop() {
    while (running_.load()) {
      const int transmissionsPerDay = [this] {
        const auto config = deviceConfigRepo_.findByDeviceId(deviceId_);
        return readTransmissionsPerDay(config ? config->config : std::string{});
      }();
      const int intervalSec = std::max(60, 86400 / std::max(1, transmissionsPerDay));

      for (int i = 0; i < intervalSec && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (!running_.load()) {
        break;
      }
      transmitNow();
    }
  }

  // Pauses generation (holds deviceFileMutex_), fetches a fresh credential, sends via
  // CoAP/DTLS, and only clears the on-disk buffers once the send is acknowledged —
  // avoids needing separate offset/ack tracking (Plano_SensorSimulatorService.md step 4).
  void transmitNow() {
    std::scoped_lock lock(deviceFileMutex_);

    rapidjson::Document payload(rapidjson::kObjectType);
    auto& alloc = payload.GetAllocator();
    payload.AddMember("device_id", rapidjson::Value(deviceId_.c_str(), alloc), alloc);
    rapidjson::Value readingsArr(rapidjson::kArrayType);

    std::vector<std::string> filesToClear;
    for (const auto& sensor : sensors_) {
      const std::string path = dataFilePath(dataDir_, deviceId_, sensor.sensorId);
      rapidjson::Document sensorReadings(rapidjson::kArrayType);
      loadJsonArray(path, sensorReadings);
      if (sensorReadings.Empty()) {
        continue;
      }
      filesToClear.push_back(path);
      for (auto& reading : sensorReadings.GetArray()) {
        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("sensor_id", rapidjson::Value(sensor.sensorId.c_str(), alloc), alloc);
        entry.AddMember("unit", rapidjson::Value(sensor.unit.c_str(), alloc), alloc);
        rapidjson::Value ts;
        ts.CopyFrom(reading["timestamp"], alloc);
        entry.AddMember("timestamp", ts, alloc);
        rapidjson::Value val;
        val.CopyFrom(reading["value"], alloc);
        entry.AddMember("value", val, alloc);
        readingsArr.PushBack(entry, alloc);
      }
    }
    payload.AddMember("readings", readingsArr, alloc);

    if (readingsArr.Empty()) {
      logger::info("Nothing to transmit", "device_id=" + deviceId_);
      return;
    }

    const auto credential = fetchActiveCredential();
    if (!credential) {
      logger::error("Skipping transmission: could not fetch active credential",
                    "device_id=" + deviceId_);
      return;
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    payload.Accept(writer);
    const std::string payloadStr = buffer.GetString();
    const std::vector<uint8_t> payloadBytes(payloadStr.begin(), payloadStr.end());

    rdws::coap::CoapDtlsClient coapClient(ingestionHost_, ingestionPort_);
    const bool sent =
        coapClient.sendConfirmable(credential->pskIdentity, credential->pskKeyPlaintext,
                                   payloadBytes);
    if (!sent) {
      logger::error("CoAP/DTLS transmission failed, keeping buffered readings",
                    "device_id=" + deviceId_);
      return;
    }

    for (const auto& path : filesToClear) {
      std::ofstream(path, std::ios::trunc);
    }
    logger::info("Transmission successful", "device_id=" + deviceId_ +
                                               " readings=" + std::to_string(readingsArr.Size()));
  }

  struct FetchedCredential {
    std::string pskIdentity;
    std::string pskKeyPlaintext;
  };

  [[nodiscard]] std::optional<FetchedCredential> fetchActiveCredential() const {
    rapidjson::Document req(rapidjson::kObjectType);
    auto& alloc = req.GetAllocator();
    req.AddMember("device_id", rapidjson::Value(deviceId_.c_str(), alloc), alloc);

    const auto result = credentialClient_->invoke("device_credential.get_active", req);
    if (!result.success) {
      logger::error("device_credential.get_active failed", result.errorMessage);
      return std::nullopt;
    }

    // responsePayload is the callee's whole handler response envelope
    // ({"success":...,"statusCode":...,"data":{...}}), not just the inner payload —
    // same shape documented/consumed by FieldServiceClient::exists().
    rapidjson::Document envelope;
    if (envelope.Parse(result.responsePayload.c_str()).HasParseError() || !envelope.IsObject()) {
      return std::nullopt;
    }
    const auto* dataObj = json::getObject(envelope, "data");
    if (dataObj == nullptr) {
      return std::nullopt;
    }
    const auto pskIdentity = json::getString(*dataObj, "psk_identity");
    const auto pskKeyHex = json::getString(*dataObj, "psk_key");
    if (!pskIdentity || !pskKeyHex) {
      return std::nullopt;
    }
    const auto keyBytes = rdws::crypto::fromHex(*pskKeyHex);
    return FetchedCredential{*pskIdentity, std::string(keyBytes.begin(), keyBytes.end())};
  }

  // ─── HTTP control API (manual trigger) ──────────────────────────────────

  void setupHttpControl() {
    httpServer_.Post(R"(/simulate/([^/?]+)/trigger)",
                     [this](const httplib::Request& req, httplib::Response& res) {
                       const std::string requestedDeviceId = req.matches[1];
                       if (requestedDeviceId != deviceId_) {
                         res.status = 404;
                         res.set_content("{\"error\":\"This instance only simulates device_id " +
                                            deviceId_ + "\"}",
                                        "application/json");
                         return;
                       }
                       std::thread([this] { transmitNow(); }).detach();
                       res.status = 202;
                       res.set_content("{\"status\":\"triggered\"}", "application/json");
                     });
  }
};

static AppSensorSimulatorService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int argc, char* argv[]) {
  std::string deviceId;
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--device-id" && i + 1 < argc) {
      deviceId = argv[++i];
    } else if (arg == "--gateway" && i + 1 < argc) {
      gatewayAddress = argv[++i];
    }
  }

  if (deviceId.empty()) {
    std::fprintf(stderr, "Usage: sensor_simulator_service --device-id <id> [--gateway <addr>]\n");
    return 1;
  }

  logger::init("sensor_simulator_service", "info", "simulator_" + deviceId);

  rdws::Config(); // loads .env for native/dev runs before plain getenv() below

  const std::string ingestionHost = getenvOrDefault("INGESTION_HOST", "localhost");
  const uint16_t ingestionPort =
      static_cast<uint16_t>(std::stoi(getenvOrDefault("INGESTION_PORT", "5684")));
  const std::string dataDir = getenvOrDefault("SIMULATOR_DATA_DIR", "./sim_data");
  const int httpControlPort =
      std::stoi(getenvOrDefault("SIMULATOR_CONTROL_PORT", "9100"));

  AppSensorSimulatorService service(deviceId, gatewayAddress, ingestionHost, ingestionPort,
                                    dataDir, httpControlPort);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize SensorSimulatorService", "device_id=" + deviceId);
    return 1;
  }
  service.run();
  return 0;
}
