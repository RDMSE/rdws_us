//
// ReadingWriterService — pure worker (Plano_Ingestion.md: "não expõe endpoints HTTP —
// é um worker puro, sem capability registrada no gateway"). No ServiceClient/gateway
// connection at all: just a direct Postgres connection and an AMQP consumer.
//
// Consumes the "sensor_readings" queue (one message per reading, published by
// IngestionService) and writes to sensor_readings, idempotently (UNIQUE(sensor_id,
// timestamp), V8 migration) — only acks after the insert is confirmed, so a crash
// mid-processing leaves the message for redelivery instead of losing it.
//

#include "../../shared/amqp/amqp_client.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/repository/SensorReadingRepository.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <rapidjson/document.h>

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <string>

namespace json = rdws::utils::json;
namespace logger = rdws::utils::logger;
using namespace rdws::database;

namespace {

std::string getenvOrDefault(const char* name, const std::string& def) {
  const char* value = std::getenv(name);
  return (value != nullptr && std::string(value).length() > 0) ? value : def;
}

} // namespace

class AppReadingWriterService {
public:
  AppReadingWriterService(std::string mqHost, uint16_t mqPort, std::string mqUser,
                          std::string mqPassword)
      : repo_(db_),
        consumer_(std::move(mqHost), mqPort, std::move(mqUser), std::move(mqPassword),
                 "sensor_readings") {}

  bool initialize() {
    db_.connect();
    return consumer_.connect();
  }

  void run() {
    running_.store(true);
    logger::info("ReadingWriterService starting", "");
    while (running_.load()) {
      (void)consumer_.consumeOne(
          [this](const std::string& body) { return handleMessage(body); }, 1000);
    }
    logger::info("ReadingWriterService stopped", "");
  }

  void shutdown() { running_.store(false); }

private:
  PostgreSQLDatabase db_;
  rdws::sensor_reading::SensorReadingRepository repo_;
  rdws::amqp::AmqpConsumer consumer_;
  std::atomic<bool> running_{false};

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

    const bool ok = repo_.insert(*sensorId, *timestamp, std::to_string(*value));
    if (!ok) {
      logger::error("ReadingWriterService: DB insert failed, leaving message unacked", body);
      return false;
    }
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

  const std::string mqHost = getenvOrDefault("RABBITMQ_HOST", "localhost");
  const uint16_t mqPort = static_cast<uint16_t>(std::stoi(getenvOrDefault("RABBITMQ_PORT", "5672")));
  const std::string mqUser = getenvOrDefault("RABBITMQ_USER", "guest");
  const std::string mqPassword = getenvOrDefault("RABBITMQ_PASSWORD", "guest");

  AppReadingWriterService service(mqHost, mqPort, mqUser, mqPassword);
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
