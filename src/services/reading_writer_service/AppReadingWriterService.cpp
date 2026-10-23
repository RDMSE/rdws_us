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
// Database failures (Plano_Ingestion.md, "Vazão e falhas do ReadingWriterService"): a failed
// write is classified by SQLSTATE. Class 22 (data exception) or 23 (integrity constraint,
// e.g. an unknown sensor_id) → the message itself is the problem (poison): it goes to the
// dead-letter queue (below). Anything else — connection lost, server shutting down, schema mismatch
// (column/table missing after a deploy ahead of its migration), unknown — is treated as the
// database being unavailable: the message is nacked with requeue and consumption pauses,
// probing with SELECT 1 and backoff (1 s → 30 s, no limit) until the database answers. A
// "the database answers, so the message is bad" probe would discard messages in both of
// those cases. The service also starts without a database and waits for it the same way,
// instead of exiting into a restart loop.
//
// Poison messages — malformed JSON, a missing required field, or a write rejected with
// SQLSTATE 22/23 — are published to "<queue>.dead" (sensor_readings.dead,
// device_telemetry.dead) wrapped as {"reason", "sqlstate", "error", "failed_at", "body"},
// with the original message as a string in "body", then acked. Kept for inspection and manual
// re-drive instead of only a log line. If that publish fails the original is requeued, never
// dropped. The writer publishes them itself instead of an x-dead-letter-exchange argument on
// the queues, which would change the arguments of existing durable queues (queue_declare
// fails with PRECONDITION_FAILED until they are recreated).
//
// RabbitMQ failures: a consumer whose connection drops (broker restart, socket error) is
// reconnected with the same kind of backoff, also at startup. Unacked deliveries go back to
// the queue when the connection closes, and the writes are idempotent.
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
#include <ctime>
#include <memory>
#include <optional>
#include <csignal>
#include <string>
#include <thread>
#include <unordered_map>

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;
using namespace rdws::database;
using Outcome = rdws::amqp::AmqpConsumer::Outcome;

class AppReadingWriterService {
public:
  AppReadingWriterService(std::string mqHost, uint16_t mqPort, std::string mqUser,
                          std::string mqPassword, int retentionDays)
      : retentionDays_(retentionDays),
        consumer_(mqHost, mqPort, mqUser, mqPassword, "sensor_readings"),
        telemetryConsumer_(mqHost, mqPort, mqUser, mqPassword, "device_telemetry"),
        deadReadings_(mqHost, mqPort, mqUser, mqPassword, "sensor_readings.dead"),
        deadTelemetry_(std::move(mqHost), mqPort, std::move(mqUser), std::move(mqPassword),
                       "device_telemetry.dead") {}

  void run() {
    running_.store(true);
    logger::info("ReadingWriterService starting", "");
    const auto onReading = [this](const std::string& body) { return handleMessage(body); };
    const auto onTelemetry = [this](const std::string& body) {
      return handleTelemetryMessage(body);
    };
    while (running_.load()) {
      if (!consumersConnected()) {
        logBurstIfAny();
        waitForBroker();
        continue;
      }
      if (dbDown_) {
        logBurstIfAny();
        waitForDatabase();
        continue;
      }
      runRetentionIfDue();
      // Poll both queues without waiting, so a backlog on one isn't throttled by the other
      // being empty (a fixed wait per queue capped a 1080-reading uplink at ~2 msg/s).
      const bool gotReading = consume(consumer_, onReading, 0);
      const bool gotTelemetry = !dbDown_ && consume(telemetryConsumer_, onTelemetry, 0);
      countBurst(gotReading, gotTelemetry);
      if (!gotReading && !gotTelemetry && !dbDown_ && consumersConnected()) {
        // Both idle: block on the socket instead of spinning. Telemetry arriving meanwhile
        // waits at most this long — latency only matters while idle, not for throughput.
        const bool gotLate = consume(consumer_, onReading, 500);
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
  // Created on the first successful connection (PostgreSQLDatabase connects in its
  // constructor and throws if the database is down); after that, the connection reopens
  // by itself on the next query (PostgreSQLDatabase::ensureConnection).
  std::unique_ptr<PostgreSQLDatabase> db_;
  std::unique_ptr<rdws::sensor_reading::SensorReadingRepository> repo_;
  std::unique_ptr<rdws::device_telemetry::DeviceTelemetryRepository> telemetryRepo_;
  std::unique_ptr<rdws::device::DeviceActivityRepository> activityRepo_;
  // Starts true: the first loop iteration connects (or waits) before consuming anything.
  bool dbDown_ = true;
  std::chrono::milliseconds dbBackoff_ = kInitialBackoff;
  std::chrono::steady_clock::time_point retryAt_; // epoch: the first probe runs right away
  std::chrono::milliseconds mqBackoff_ = kInitialBackoff;
  std::chrono::steady_clock::time_point mqRetryAt_;
  // Last time this process touched each device's last_seen: skips the UPDATE for the other
  // readings of the same uplink cycle (the SQL has its own guard, this saves the round trip)
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastTouch_;
  rdws::amqp::AmqpConsumer consumer_;
  rdws::amqp::AmqpConsumer telemetryConsumer_;
  // Connect lazily on the first poison message.
  rdws::amqp::AmqpProducer deadReadings_;
  rdws::amqp::AmqpProducer deadTelemetry_;
  std::atomic<bool> running_{false};
  // Messages consumed since the queues were last idle — one summary line per uplink burst
  // instead of a log line per message (counts include discarded/unacked ones).
  std::chrono::steady_clock::time_point burstStart_;
  unsigned int burstReadings_ = 0;
  unsigned int burstTelemetry_ = 0;

  static constexpr auto kTouchInterval = std::chrono::minutes(1);
  static constexpr auto kRetentionInterval = std::chrono::hours(24);
  static constexpr auto kInitialBackoff = std::chrono::milliseconds(1000);
  static constexpr auto kMaxBackoff = std::chrono::milliseconds(30000);

  // Sleeps until `until` in short steps, so shutdown() is prompt. False if shutting down.
  bool sleepUntil(std::chrono::steady_clock::time_point until) const {
    while (running_.load() && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return running_.load();
  }

  bool consumersConnected() const {
    return consumer_.isConnected() && telemetryConsumer_.isConnected();
  }

  // consumeOne() on a connected consumer, logging the moment its connection drops.
  bool consume(rdws::amqp::AmqpConsumer& consumer,
               const rdws::amqp::AmqpConsumer::MessageHandler& handler, unsigned int timeoutMs) {
    if (!consumer.isConnected()) {
      return false;
    }
    const bool got = consumer.consumeOne(handler, timeoutMs);
    if (!consumer.isConnected()) {
      logger::error("ReadingWriterService: RabbitMQ connection lost",
                    "queue=" + consumer.queueName() + " error=" + consumer.lastError());
    }
    return got;
  }

  // Called while a consumer is disconnected: sleeps until mqRetryAt_, then reconnects
  // whichever consumer is down; on failure doubles the backoff (capped).
  void waitForBroker() {
    if (!sleepUntil(mqRetryAt_)) {
      return;
    }
    const bool ok = (consumer_.isConnected() || consumer_.connect()) &&
                    (telemetryConsumer_.isConnected() || telemetryConsumer_.connect());
    if (ok) {
      logger::info("ReadingWriterService: RabbitMQ connected", "");
      mqBackoff_ = kInitialBackoff;
      return;
    }
    logger::warn("ReadingWriterService: RabbitMQ unavailable, retrying",
                 "retry_in_ms=" + std::to_string(mqBackoff_.count()));
    mqRetryAt_ = std::chrono::steady_clock::now() + mqBackoff_;
    mqBackoff_ = std::min(mqBackoff_ * 2, kMaxBackoff);
  }

  // True iff the database answers a SELECT 1 (connecting first if needed).
  bool probeDatabase() {
    try {
      if (!db_) {
        db_ = std::make_unique<PostgreSQLDatabase>();
        repo_ = std::make_unique<rdws::sensor_reading::SensorReadingRepository>(*db_);
        telemetryRepo_ = std::make_unique<rdws::device_telemetry::DeviceTelemetryRepository>(*db_);
        activityRepo_ = std::make_unique<rdws::device::DeviceActivityRepository>(*db_);
      }
      (void)db_->execQuery("SELECT 1");
      return true;
    } catch (const std::exception& e) {
      lastDbError_ = e.what();
      return false;
    }
  }

  // Next attempt in dbBackoff_, which then doubles (capped). Only a successful write resets
  // it (writeSucceeded): an error the SELECT 1 probe can't see — e.g. a missing column —
  // keeps backing off instead of retrying once a second.
  void scheduleRetry() {
    retryAt_ = std::chrono::steady_clock::now() + dbBackoff_;
    dbBackoff_ = std::min(dbBackoff_ * 2, kMaxBackoff);
  }

  // Called while dbDown_: sleeps until retryAt_, then probes; resumes consuming if the
  // database answers, otherwise schedules another try.
  void waitForDatabase() {
    if (!sleepUntil(retryAt_)) {
      return;
    }
    if (probeDatabase()) {
      logger::info("ReadingWriterService: database available, consuming", "");
      dbDown_ = false;
      return;
    }
    logger::warn("ReadingWriterService: database unavailable, retrying",
                 "retry_in_ms=" + std::to_string(dbBackoff_.count()) + " error=" + lastDbError_);
    scheduleRetry();
  }

  // SQLSTATE classes that point at the message, not at the database: redelivering it would
  // fail the same way forever.
  static bool isPoison(const DatabaseError& e) {
    const auto& state = e.sqlstate();
    return state.starts_with("22") || state.starts_with("23");
  }

  // Publishes a poison message to its .dead queue (see the header) and acks the original;
  // requeues it instead if the dead-letter publish fails.
  static Outcome deadLetter(rdws::amqp::AmqpProducer& dead, const std::string& body,
                            const std::string& reason, const std::string& sqlstate,
                            const std::string& error) {
    char failedAt[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(failedAt, sizeof(failedAt), "%Y-%m-%dT%H:%M:%SZ", &utc);

    rapidjson::Document envelope(rapidjson::kObjectType);
    auto& alloc = envelope.GetAllocator();
    envelope.AddMember("reason", rapidjson::Value(reason.c_str(), alloc), alloc);
    envelope.AddMember("sqlstate", rapidjson::Value(sqlstate.c_str(), alloc), alloc);
    envelope.AddMember("error", rapidjson::Value(error.c_str(), alloc), alloc);
    envelope.AddMember("failed_at", rapidjson::Value(failedAt, alloc), alloc);
    envelope.AddMember("body", rapidjson::Value(body.c_str(), alloc), alloc);

    if (!dead.publish(json::docToString(envelope))) {
      logger::error("ReadingWriterService: dead-letter publish failed, requeueing",
                    "reason=" + reason + " error=" + dead.lastError());
      return Outcome::Requeue;
    }
    logger::error("ReadingWriterService: poison message dead-lettered",
                  "reason=" + reason + " sqlstate=" + sqlstate + " error=" + error + " body=" + body);
    return Outcome::Ack;
  }

  // A write threw. Poison → dead-letter it; anything else → requeue and pause consuming.
  Outcome onWriteFailure(const char* what, rdws::amqp::AmqpProducer& dead, const std::string& body,
                         const DatabaseError& e) {
    if (isPoison(e)) {
      return deadLetter(dead, body, std::string(what) + "_rejected", e.sqlstate(), e.what());
    }
    logger::error(std::string("ReadingWriterService: ") + what +
                      " failed, database unavailable — requeueing and pausing",
                  "sqlstate=" + e.sqlstate() + " error=" + e.what());
    dbDown_ = true;
    scheduleRetry();
    return Outcome::Requeue;
  }

  void writeSucceeded() { dbBackoff_ = kInitialBackoff; }

  std::string lastDbError_;

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
    try {
      if (telemetryRepo_->deleteOlderThan(retentionDays_)) {
        logger::info("ReadingWriterService: device_telemetry retention ran",
                     "older_than_days=" + std::to_string(retentionDays_));
        return;
      }
      logger::error("ReadingWriterService: device_telemetry retention failed", "");
    } catch (const std::exception& e) {
      logger::error("ReadingWriterService: device_telemetry retention failed", e.what());
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
    try {
      if (!activityRepo_->touchLastSeen(deviceId)) {
        logger::warn("ReadingWriterService: last_seen update failed", "device_id=" + deviceId);
      }
    } catch (const std::exception& e) {
      logger::warn("ReadingWriterService: last_seen update failed",
                   "device_id=" + deviceId + " error=" + e.what());
    }
  }

  // Acks once the write succeeded. A parse/format error dead-letters it (a malformed message
  // can never become valid on redelivery, so retrying forever would just wedge the queue);
  // only a database outage requeues it (onWriteFailure).
  Outcome handleMessage(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      return deadLetter(deadReadings_, body, "malformed", "", "not a JSON object");
    }

    const auto sensorId = json::getString(doc, "sensor_id");
    const auto timestamp = json::getString(doc, "timestamp");
    const auto value = json::getDouble(doc, "value");
    if (!sensorId || !timestamp || !value) {
      return deadLetter(deadReadings_, body, "missing_field", "",
                        "sensor_id, timestamp and value are required");
    }

    // Optional: only SenML payloads carry fl_ (Plano_Telemetria.md, DP3); legacy JSON doesn't.
    const int flags = json::getInt(doc, "flags").value_or(0);

    try {
      (void)repo_->insert(*sensorId, *timestamp, std::to_string(*value), flags);
    } catch (const DatabaseError& e) {
      return onWriteFailure("insert", deadReadings_, body, e);
    }
    writeSucceeded();
    if (const auto deviceId = json::getString(doc, "device_id")) {
      touchLastSeen(*deviceId);
    }
    return Outcome::Ack;
  }

  // {"device_id": "12", "timestamp": "...Z", "data": {"seq": 812, "rssi": -67, ...}}
  Outcome handleTelemetryMessage(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
      return deadLetter(deadTelemetry_, body, "malformed", "", "not a JSON object");
    }

    const auto deviceId = json::getString(doc, "device_id");
    const auto timestamp = json::getString(doc, "timestamp");
    const auto* data = json::getObject(doc, "data");
    if (!deviceId || !timestamp || data == nullptr) {
      return deadLetter(deadTelemetry_, body, "missing_field", "",
                        "device_id, timestamp and data are required");
    }

    try {
      (void)telemetryRepo_->upsert(*deviceId, *timestamp, json::docToString(*data));
    } catch (const DatabaseError& e) {
      return onWriteFailure("telemetry_upsert", deadTelemetry_, body, e);
    }
    writeSucceeded();
    touchLastSeen(*deviceId);
    return Outcome::Ack;
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

  service.run();
  return 0;
}
