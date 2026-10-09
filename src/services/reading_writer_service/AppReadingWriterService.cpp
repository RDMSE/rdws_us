//
// ReadingWriterService — pure worker (Plano_Ingestion.md: "não expõe endpoints HTTP —
// é um worker puro, sem capability registrada no gateway"). No ServiceClient/gateway
// connection at all: just a direct Postgres connection and an AMQP consumer.
//
// Consumes the "sensor_readings" queue (one message per reading, published by
// IngestionService; optional "flags" = SenML fl_ bitmask, V10) and writes to
// sensor_readings, idempotently (UNIQUE(sensor_id, timestamp), V8 migration) — only acks
// after the insert is confirmed, so a crash mid-processing leaves the message for
// redelivery instead of losing it.
//
// Also consumes the "device_telemetry" queue (device diagnostics from SenML packs,
// Plano_Telemetria.md D5) and upserts into device_telemetry (V11), same ack policy. Every
// accepted message also refreshes devices.last_seen (V14), at most once a minute per device.
//
// Housekeeping: device_telemetry rows older than TELEMETRY_RETENTION_DAYS (default 90,
// Plano_Telemetria.md Fase 3 R4) are deleted at startup and then once a day.
//

#include "../../shared/amqp/amqp_client.h"
#include "../../shared/config/config.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/DeviceActivityRepository.h"
#include "../../shared/repository/DeviceTelemetryRepository.h"
#include "../../shared/repository/SensorReadingRepository.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <csignal>
#include <string>
#include <unordered_map>

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;
using namespace rdws::database;

class AppReadingWriterService {
public:
  AppReadingWriterService(std::string mqHost, uint16_t mqPort, std::string mqUser,
                          std::string mqPassword, int retentionDays)
      : retentionDays_(retentionDays), repo_(db_), telemetryRepo_(db_), activityRepo_(db_),
        consumer_(mqHost, mqPort, mqUser, mqPassword, "sensor_readings"),
        telemetryConsumer_(std::move(mqHost), mqPort, std::move(mqUser), std::move(mqPassword),
                           "device_telemetry") {}

  bool initialize() {
    db_.connect();
    return consumer_.connect() && telemetryConsumer_.connect();
  }

  void run() {
    running_.store(true);
    logger::info("ReadingWriterService starting", "");
    const auto onReading = [this](const std::string& body) { return handleMessage(body); };
    const auto onTelemetry = [this](const std::string& body) {
      return handleTelemetryMessage(body);
    };
    while (running_.load()) {
      runRetentionIfDue();
      // Poll both queues without waiting, so a backlog on one isn't throttled by the other
      // being empty (a fixed wait per queue capped a 1080-reading uplink at ~2 msg/s).
      const bool gotReading = consumer_.consumeOne(onReading, 0);
      const bool gotTelemetry = telemetryConsumer_.consumeOne(onTelemetry, 0);
      countBurst(gotReading, gotTelemetry);
      if (!gotReading && !gotTelemetry) {
        // Both idle: block on the socket instead of spinning. Telemetry arriving meanwhile
        // waits at most this long — latency only matters while idle, not for throughput.
        const bool gotLate = consumer_.consumeOne(onReading, 500);
        countBurst(gotLate, false);
        if (!gotLate) {
          logBurstIfAny(); // a full idle wait ends the burst
        }
      }
    }
    logBurstIfAny();
    logger::info("ReadingWriterService stopped", "");
  }

  void shutdown() { running_.store(false); }

private:
  int retentionDays_;
  std::optional<std::chrono::steady_clock::time_point> lastRetention_;
  PostgreSQLDatabase db_;
  rdws::sensor_reading::SensorReadingRepository repo_;
  rdws::device_telemetry::DeviceTelemetryRepository telemetryRepo_;
  rdws::device::DeviceActivityRepository activityRepo_;
  // Last time this process touched each device's last_seen: skips the UPDATE for the other
  // readings of the same uplink cycle (the SQL has its own guard, this saves the round trip)
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastTouch_;
  rdws::amqp::AmqpConsumer consumer_;
  rdws::amqp::AmqpConsumer telemetryConsumer_;
  std::atomic<bool> running_{false};
  // Messages consumed since the queues were last idle — one summary line per uplink burst
  // instead of a log line per message (counts include discarded/unacked ones).
  std::chrono::steady_clock::time_point burstStart_;
  unsigned int burstReadings_ = 0;
  unsigned int burstTelemetry_ = 0;

  static constexpr auto kTouchInterval = std::chrono::minutes(1);
  static constexpr auto kRetentionInterval = std::chrono::hours(24);

  void countBurst(bool gotReading, bool gotTelemetry) {
    if ((gotReading || gotTelemetry) && burstReadings_ == 0 && burstTelemetry_ == 0) {
      burstStart_ = std::chrono::steady_clock::now();
    }
    burstReadings_ += gotReading ? 1 : 0;
    burstTelemetry_ += gotTelemetry ? 1 : 0;
  }

  void logBurstIfAny() {
    if (burstReadings_ == 0 && burstTelemetry_ == 0) {
      return;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - burstStart_)
                               .count();
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.1f", static_cast<double>(elapsedMs) / 1000.0);
    logger::info("ReadingWriterService: queues drained",
                 "readings=" + std::to_string(burstReadings_) +
                     " telemetry=" + std::to_string(burstTelemetry_) + " seconds=" + seconds);
    burstReadings_ = 0;
    burstTelemetry_ = 0;
  }

  // At startup and then daily. A failure only logs and is retried on the next day's run.
  void runRetentionIfDue() {
    const auto now = std::chrono::steady_clock::now();
    if (lastRetention_ && now - *lastRetention_ < kRetentionInterval) {
      return;
    }
    lastRetention_ = now;
    if (telemetryRepo_.deleteOlderThan(retentionDays_)) {
      logger::info("ReadingWriterService: device_telemetry retention ran",
                   "older_than_days=" + std::to_string(retentionDays_));
    } else {
      logger::error("ReadingWriterService: device_telemetry retention failed", "");
    }
  }

  // Best effort: a failed last_seen update only logs, it never holds back the data write.
  void touchLastSeen(const std::string& deviceId) {
    const auto now = std::chrono::steady_clock::now();
    auto [it, isNew] = lastTouch_.try_emplace(deviceId, now);
    if (!isNew && now - it->second < kTouchInterval) {
      return;
    }
    it->second = now;
    if (!activityRepo_.touchLastSeen(deviceId)) {
      logger::warn("ReadingWriterService: last_seen update failed", "device_id=" + deviceId);
    }
  }

  // Returns true (ack) iff the write succeeded — a parse/format error also acks
  // (a malformed message can never become valid on redelivery, so retrying forever
  // would just wedge the queue); only a DB failure leaves it unacked for retry.
  bool handleMessage(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      logger::error("ReadingWriterService: malformed message, discarding", body);
      return true;
    }

    const auto sensorId = json::getString(doc, "sensor_id");
    const auto timestamp = json::getString(doc, "timestamp");
    const auto value = json::getDouble(doc, "value");
    if (!sensorId || !timestamp || !value) {
      logger::error("ReadingWriterService: message missing required field, discarding", body);
      return true;
    }

    // Optional: only SenML payloads carry fl_ (Plano_Telemetria.md, DP3); legacy JSON doesn't.
    const int flags = json::getInt(doc, "flags").value_or(0);

    const bool ok = repo_.insert(*sensorId, *timestamp, std::to_string(*value), flags);
    if (!ok) {
      logger::error("ReadingWriterService: DB insert failed, leaving message unacked", body);
      return false;
    }
    if (const auto deviceId = json::getString(doc, "device_id")) {
      touchLastSeen(*deviceId);
    }
    return true;
  }

  // {"device_id": "12", "timestamp": "...Z", "data": {"seq": 812, "rssi": -67, ...}}
  bool handleTelemetryMessage(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      logger::error("ReadingWriterService: malformed telemetry message, discarding", body);
      return true;
    }

    const auto deviceId = json::getString(doc, "device_id");
    const auto timestamp = json::getString(doc, "timestamp");
    const auto* data = json::getObject(doc, "data");
    if (!deviceId || !timestamp || data == nullptr) {
      logger::error("ReadingWriterService: telemetry message missing required field, discarding",
                    body);
      return true;
    }

    if (!telemetryRepo_.upsert(*deviceId, *timestamp, json::docToString(*data))) {
      logger::error("ReadingWriterService: telemetry upsert failed, leaving message unacked",
                    body);
      return false;
    }
    touchLastSeen(*deviceId);
    return true;
  }
};

static AppReadingWriterService* gService = nullptr;

void signalHandler(int sig) {
  if (gService && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(int /*argc*/, char* /*argv*/[]) {
  logger::init("reading_writer_service", "info", "reading_writer_001");

  rdws::Config(); // loads .env for native/dev runs before plain getenv() below

  const std::string mqHost = rdws::Config::getEnvVarOrDefault("RABBITMQ_HOST", "localhost");
  const uint16_t mqPort = static_cast<uint16_t>(std::stoi(rdws::Config::getEnvVarOrDefault("RABBITMQ_PORT", "5672")));
  const std::string mqUser = rdws::Config::getEnvVarOrDefault("RABBITMQ_USER", "guest");
  const std::string mqPassword = rdws::Config::getEnvVarOrDefault("RABBITMQ_PASSWORD", "guest");

  const int retentionDays =
      std::max(1, std::stoi(rdws::Config::getEnvVarOrDefault("TELEMETRY_RETENTION_DAYS", "90")));

  AppReadingWriterService service(mqHost, mqPort, mqUser, mqPassword, retentionDays);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize ReadingWriterService", "");
    return 1;
  }
  service.run();
  return 0;
}
