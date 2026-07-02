#pragma once

#include "../../shared/utils/metrics.h"
#include "../Config/GatewayConfig.h"
#include "EventBus.h"
#include "EventRouter.h"
#include "ServiceRegistry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <rapidjson/document.h>
#include <string>
#include <thread>

namespace servicegateway {

enum class RequestState : std::uint8_t { QUEUED, IN_FLIGHT, COMPLETED, FAILED, TIMED_OUT };

struct ClientConnection {
  int socketFd;
  std::string address;
  std::string connectionType; // "tcp" or "unix"
  std::string serviceId;      // Empty until identified
  bool identified = false;
  std::chrono::time_point<std::chrono::steady_clock> connectedAt = std::chrono::steady_clock::now();
};

struct PendingRequest {
  std::string requestId;
  std::string targetServiceId;
  std::string capability;
  std::string requestPayload;
  std::string responsePayload;
  std::string errorMessage;
  RequestState state = RequestState::QUEUED;
  int statusCode = 202;
  std::chrono::milliseconds timeout = std::chrono::milliseconds(30000);
  std::chrono::time_point<std::chrono::steady_clock> createdAt = std::chrono::steady_clock::now();
  std::chrono::time_point<std::chrono::steady_clock> updatedAt = createdAt;
};

using MessageHandler =
    std::function<void(const std::string& serviceId, const rapidjson::Document& message)>;

class ServiceGateway {
private:
  // Configuration
  int tcpPort = 8080;
  std::string unixSocketPath = "/tmp/service_gateway.sock";

  // Core components
  ServiceRegistry registry;

  // Network threads
  std::thread tcpListener;
  std::thread unixListener;
  std::atomic<bool> running{false};
  std::atomic<int> tcpServerFd{-1}; // closed in stop() to unblock accept()
  std::atomic<int> unixServerFd{-1};

  // Connection management
  std::map<int, ClientConnection> activeConnections;
  mutable std::mutex connectionsMutex;

  // Request management
  std::map<std::string, PendingRequest> pendingRequests;
  mutable std::mutex requestsMutex;
  std::atomic<uint64_t> requestIdCounter{0};
  std::map<std::string, std::shared_ptr<std::condition_variable>> responseWaiters_;

  // Message handlers
  std::map<std::string, MessageHandler> messageHandlers;

  // Health check thread
  std::thread healthCheckThread;

  // Periodic metrics snapshot thread (publishes metrics.snapshot to EventBus)
  std::thread metricsSnapshotThread_;

  // Per-capability metrics
  rdws::metrics::MetricsTracker metrics_;

  // Dynamic routing rules
  EventRouter router_;

  // Internal pub/sub bus
  EventBus bus_;

  // Runtime configuration
  GatewayConfig config_;

public:
  explicit ServiceGateway(int port = 8080, std::string unixSocket = "/tmp/service_gateway.sock",
                          std::string routesFile = "", std::string configFile = "");
  ServiceGateway() = delete;
  ~ServiceGateway();

  // Lifecycle management
  bool start();
  void stop();
  bool isRunning() const {
    return running.load();
  }

  // Network listeners
  void startTcpListener();
  void startUnixListener();
  void startHealthChecker();

  // Connection management
  void handleNewConnection(int clientFd, const std::string& address, const std::string& type);
  void handleClientMessage(int clientFd, const std::string& message);
  void closeConnection(int clientFd);

  // Protocol handlers
  bool handleIdentifyMessage(int clientFd, const rapidjson::Document& message);
  bool handlePingMessage(int clientFd, const rapidjson::Document& message);
  bool handleResponseMessage(int clientFd, const rapidjson::Document& message);

  // Request routing
  std::string sendRequest(const std::string& capability, const rapidjson::Document& requestData,
                          LoadBalancingStrategy strategy = LoadBalancingStrategy::LEAST_LOADED);
  bool sendDirectRequest(const std::string& serviceId, const rapidjson::Document& requestData);
  rapidjson::Document getRequestStatus(const std::string& requestId) const;
  PendingRequest
  waitForResponse(const std::string& requestId,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));

  // Monitoring and management
  rapidjson::Document getGatewayStatus() const;
  rapidjson::Document getConnectionStatus() const;
  rapidjson::Document getMetrics() const;
  rapidjson::Document getHealth() const;
  void recordMetric(const std::string& capability, double latencyMs, bool success,
                    bool timedOut = false);
  size_t getActiveConnectionCount() const;
  size_t getTrackedRequestCount() const;

  // Message handlers registration
  void setMessageHandler(const std::string& messageType, MessageHandler handler);

  // Registry access
  ServiceRegistry& getRegistry() {
    return registry;
  }

  // EventRouter access
  EventRouter& getEventRouter() {
    return router_;
  }

  // EventBus access
  EventBus& getBus() {
    return bus_;
  }

  // GatewayConfig access
  GatewayConfig& getConfig() {
    return config_;
  }
  const GatewayConfig& getConfig() const {
    return config_;
  }

  const ServiceRegistry& getRegistry() const {
    return registry;
  }

private:
  // Helper methods
  static bool sendMessage(int socketFd, const std::string& message);
  static std::string requestStateToString(RequestState state);
  std::string generateRequestId();

  void cleanupExpiredRequests();
  void performHealthCheck();

  // Socket helpers
  static int createTcpSocket();
  static int createUnixSocket();
  void setSocketNonBlocking(int socketFd);
};

// Compatibility alias for phase-in rename Broker -> Gateway.
using ServiceBroker = ServiceGateway;

} // namespace servicegateway

// Namespace-level compatibility alias for phase-in rename Broker -> Gateway.
namespace servicebroker = servicegateway;
