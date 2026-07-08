//
// PersistenceService — capabilities: persistence.save.request, persistence.save.metrics
//
// Receives gateway lifecycle events forwarded by the ServiceGateway EventBus bridge
// and batch-upserts them into PostgreSQL (request_history, capability_metrics).
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/database/postgresql_database.h"
#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/capability_router.h"
#include "../../shared/utils/response_helper.h"
#include "../../shared/utils/profiler.h"

#include "../../shared/utils/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <memory>
#include <mutex>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace servicegateway;
using namespace rdws::database;
using rdws::utils::ResponseHelper;
namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

// ─── Buffer types ─────────────────────────────────────────────────────────────

struct RequestRecord {
  std::string requestId;
  std::string capability;
  std::string serviceId;
  bool success;
  int latencyMs;
};

struct MetricsRecord {
  std::string metricsJson; // full metrics.toJson() serialized
  std::string snapshotAt;
};

// ─── PersistenceService ───────────────────────────────────────────────────────

class AppPersistenceService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  std::atomic<bool> running{false};

  // Incoming request buffer (flushed every N items or T seconds)
  std::deque<RequestRecord> requestBuffer_;
  std::deque<MetricsRecord> metricsBuffer_;
  std::mutex bufferMutex_;
  std::thread flushThread_;

  static constexpr size_t kFlushBatchSize = 50;
  static constexpr int kFlushIntervalSec = 10;

public:
  AppPersistenceService(const std::string& serviceId, const std::string& machineName,
                     std::string broker)
      : gatewayAddress(std::move(broker)) {
    identity.machineName = machineName;
    identity.serviceName = "persistence_service";
    identity.serviceId = serviceId;
    identity.version = "v1.0.0";
    identity.environment = "prod";
    identity.maxConcurrent = 20;
    identity.capabilities = {"persistence.save.request", "persistence.save.metrics"};
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
    logger::info("PersistenceService starting", identity.serviceId);

    flushThread_ = std::thread([this]() {
      while (running.load()) {
        for (int i = 0; i < kFlushIntervalSec && running.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        flushBuffers();
      }
      flushBuffers(); // drain on shutdown
    });

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

    logger::info("PersistenceService stopped", identity.serviceId);
  }

  void shutdown() {
    running.store(false);
    if (client) {
      client->stop();
    }
    if (flushThread_.joinable()) {
      flushThread_.join();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) {
    const auto cap = json::getString(request, "capability").value_or("");

    const std::unordered_map<std::string,
                              rdws::utils::CapabilityHandler<AppPersistenceService>>
        handlers = {
            {"persistence.save.request",
             [this](const rapidjson::Document& req, AppPersistenceService&) {
               return handleSaveRequest(req);
             }},
            {"persistence.save.metrics",
             [this](const rapidjson::Document& req, AppPersistenceService&) {
               return handleSaveMetrics(req);
             }},
        };

    try {
      rdws::utils::Profiler profiler(identity.serviceId);
      auto t = profiler.scoped(cap);
      return rdws::utils::dispatchCapability(cap, request, *this, handlers);
    } catch (const std::exception& e) {
      logger::error("Request error", identity.serviceId + " " + e.what());
      return ResponseHelper::returnErrorDoc(std::string("Internal error: ") + e.what(), 500);
    }
  }

  // Enqueue a single request.completed event
  //
  // "capability" no payload é a capability do PersistenceService (persistence.save.request
  // — usada só pra dispatch, ver ServiceGateway::start()); a capability da requisição
  // original registrada no histórico vem em "originalCapability".
  rapidjson::Document handleSaveRequest(const rapidjson::Document& req) {
    RequestRecord r{
        .requestId = json::getString(req, "requestId").value_or(std::string{}),
        .capability = json::getString(req, "originalCapability").value_or(std::string{}),
        .serviceId = json::getString(req, "serviceId").value_or(std::string{}),
        .success = json::getBool(req, "success").value_or(false),
        .latencyMs = json::getInt(req, "latencyMs").value_or(0),
    };

    if (r.requestId.empty()) {
      return ResponseHelper::returnErrorDoc("Missing requestId");
    }

    {
      std::scoped_lock lock(bufferMutex_);
      requestBuffer_.push_back(std::move(r));
      if (requestBuffer_.size() >= kFlushBatchSize) {
        flushRequestBuffer();
      }
    }
    return ResponseHelper::returnSuccessDoc();
  }

  // Enqueue a metrics.snapshot event
  rapidjson::Document handleSaveMetrics(const rapidjson::Document& req) {
    MetricsRecord m;
    m.snapshotAt = json::getString(req, "snapshotAt").value_or("");
    m.metricsJson = json::docToString(req);

    {
      std::scoped_lock lock(bufferMutex_);
      metricsBuffer_.push_back(std::move(m));
      if (metricsBuffer_.size() >= kFlushBatchSize) {
        flushMetricsBuffer();
      }
    }
    return ResponseHelper::returnSuccessDoc();
  }

  // Called by flush thread (already holds bufferMutex_ when called from handlers,
  // or acquires it in the periodic path)
  void flushBuffers() {
    std::scoped_lock lock(bufferMutex_);
    flushRequestBuffer();
    flushMetricsBuffer();
  }

  // Must be called with bufferMutex_ held
  void flushRequestBuffer() {
    if (requestBuffer_.empty()) {
      return;
    }

    std::vector<RequestRecord> batch;
    batch.reserve(requestBuffer_.size());
    while (!requestBuffer_.empty()) {
      batch.push_back(std::move(requestBuffer_.front()));
      requestBuffer_.pop_front();
    }

    try {
      PostgreSQLDatabase db;
      db.connect();
      for (const auto& r : batch) {
        db.execCommand("INSERT INTO request_history "
                       "(request_id, capability, service_id, success, latency_ms) "
                       "VALUES ($1, $2, $3, $4, $5) "
                       "ON CONFLICT (request_id) DO NOTHING",
                       {r.requestId, r.capability, r.serviceId, r.success ? "true" : "false",
                        std::to_string(r.latencyMs)});
      }
      logger::info("Flushed request records", identity.serviceId + " count=" + std::to_string(batch.size()));
    } catch (const std::exception& e) {
      logger::error("Flush error", identity.serviceId + " " + e.what());
    }
  }

  // Must be called with bufferMutex_ held
  void flushMetricsBuffer() {
    if (metricsBuffer_.empty()) {
      return;
    }

    std::vector<MetricsRecord> batch;
    batch.reserve(metricsBuffer_.size());
    while (!metricsBuffer_.empty()) {
      batch.push_back(std::move(metricsBuffer_.front()));
      metricsBuffer_.pop_front();
    }

    try {
      PostgreSQLDatabase db;
      db.connect();
      for (const auto& [metricsJson, snapshotAt] : batch) {
        // Parse the metrics JSON and insert per-capability rows
        rapidjson::Document doc;
        doc.Parse(metricsJson.c_str());
        if (doc.HasParseError() || !doc.IsObject()) {
          continue;
        }

        // metrics.snapshot tem a forma {"capabilities": [{...},{...}], "snapshotAt": "..."}
        // — um array de objetos, um por capability (ver MetricsTracker::toJson) —, não um
        // objeto plano {"cap": {...}}.
        const auto* capabilitiesArray = json::getArray(doc, "capabilities");
        if (capabilitiesArray == nullptr) {
          continue;
        }

        for (const auto& stats : capabilitiesArray->GetArray()) {
          const std::string key = json::getString(stats, "capability").value_or("unknown");
          auto getNum = [&](const char* k) -> std::string {
            if (const auto valInt = json::getInt(stats, k); valInt.has_value()) {
              return std::to_string(valInt.value());
            }
            if (const auto valInt64 = json::getInt64(stats, k); valInt64.has_value()) {
              return std::to_string(valInt64.value());
            }
            if (const auto valDouble = json::getDouble(stats, k); valDouble.has_value()) {
              return std::to_string(valDouble.value());
            }
            return "0";
          };

          db.execCommand("INSERT INTO capability_metrics "
                         "(capability, window_start, request_count, error_count, timeout_count, "
                         " avg_latency_ms, p99_latency_ms, min_latency_ms, max_latency_ms) "
                         "VALUES ($1, to_timestamp($2::bigint), $3, $4, $5, $6, $7, $8, $9)",
                         {key, snapshotAt.empty() ? "0" : snapshotAt, getNum("requestCount"),
                          getNum("errorCount"), getNum("timeoutCount"), getNum("avgLatencyMs"),
                          getNum("p99LatencyMs"), getNum("minLatencyMs"), getNum("maxLatencyMs")});
        }
      }
      logger::info("Flushed metrics snapshots", identity.serviceId + " count=" + std::to_string(batch.size()));
    } catch (const std::exception& e) {
      logger::error("Metrics flush error", identity.serviceId + " " + e.what());
    }
  }
};

static AppPersistenceService* gService = nullptr;

void signalHandler(const int sig) {
  if ((gService != nullptr) && (sig == SIGTERM || sig == SIGINT)) {
    gService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  std::string serviceId = "persistence_001";
  std::string machineName = "localhost";
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";

  if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc >= 2 && std::string(argv[1]) == "--dev") {
    serviceId = "persistence_dev";
    machineName = "dev-machine";
  }

  logger::init("persistence_service", "info", serviceId);

  AppPersistenceService service(serviceId, machineName, gatewayAddress);
  gService = &service;
  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    logger::error("Failed to initialize PersistenceService");
    return 1;
  }
  service.run();
  return 0;
}
