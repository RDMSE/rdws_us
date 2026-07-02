#include "ServiceGateway.h"

#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"

#include <arpa/inet.h>
#include <cmath>
#include <netinet/in.h>
#include <ranges>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

namespace servicegateway {

ServiceGateway::ServiceGateway(const int port, std::string unixSocket, std::string routesFile,
                               std::string configFile)
    : tcpPort(port), unixSocketPath(std::move(unixSocket)), router_(std::move(routesFile)),
      config_(configFile.empty() ? GatewayConfig{} : GatewayConfig::fromFile(configFile)) {

  // Set up default message handlers
  setMessageHandler("IDENTIFY",
                    [this](const std::string& serviceId, const rapidjson::Document& message) {
                      // Already handled in handleIdentifyMessage
                    });

  setMessageHandler("PING",
                    [this](const std::string& serviceId, const rapidjson::Document& message) {
                      registry.pingService(serviceId);
                    });
}

ServiceGateway::~ServiceGateway() {
  stop();
}

bool ServiceGateway::start() {
  if (running.load()) {
    logger::warn("ServiceGateway is already running");
    return false;
  }

  running.store(true);

  bus_.start();

  logger::info("Starting ServiceGateway", "tcp=" + std::to_string(tcpPort) + " unix=" + unixSocketPath);

  // Start network listeners
  tcpListener = std::thread(&ServiceGateway::startTcpListener, this);
  unixListener = std::thread(&ServiceGateway::startUnixListener, this);
  healthCheckThread = std::thread(&ServiceGateway::startHealthChecker, this);

  // Bridge request.completed → persistence.save.request (fire-and-forget)
  bus_.subscribe("request.completed", [this](const std::string&, const rapidjson::Document& ev) {
    // Skip forwarding persistence-internal requests to avoid loops
    const auto capability = json::getString(ev, "capability");
    if (capability.has_value() && capability.value().rfind("persistence.", 0) == 0) {
      return;
    }
    rapidjson::Document payload;
    payload.CopyFrom(ev, payload.GetAllocator());
    sendRequest("persistence.save.request", payload, LoadBalancingStrategy::ROUND_ROBIN);
  });

  // Bridge metrics.snapshot → persistence.save.metrics
  bus_.subscribe("metrics.snapshot", [this](const std::string&, const rapidjson::Document& ev) {
    rapidjson::Document payload;
    payload.CopyFrom(ev, payload.GetAllocator());
    sendRequest("persistence.save.metrics", payload, LoadBalancingStrategy::ROUND_ROBIN);
  });

  // Publish metrics.snapshot every 60 seconds
  metricsSnapshotThread_ = std::thread([this]() {
    while (running.load()) {
      for (int i = 0; i < 60 && running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      if (!running.load()) {
        break;
      }
      rapidjson::Document snap = metrics_.toJson();
      snap.AddMember(
          "snapshotAt",
          rapidjson::Value(std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count())
                               .c_str(),
                           snap.GetAllocator()),
          snap.GetAllocator());
      bus_.publish("metrics.snapshot", snap);
    }
  });

  logger::info("ServiceGateway started successfully");
  return true;
}

void ServiceGateway::stop() {
  if (!running.load()) {
    return;
  }

  logger::info("Stopping ServiceGateway");
  running.store(false);

  // Unblock select()/accept() in listener threads.
  if (const int fd = tcpServerFd.exchange(-1); fd != -1) {
    close(fd);
  }
  if (const int fd = unixServerFd.exchange(-1); fd != -1) {
    close(fd);
  }

  // Close all active connections (scoped so the mutex is released before joining).
  {
    std::scoped_lock lock(connectionsMutex);
    for (const auto& fd : activeConnections | std::views::keys) {
      close(fd);
    }
    activeConnections.clear();
  }

  // Wait for threads to finish (mutex NOT held here).
  if (tcpListener.joinable()) {
    tcpListener.join();
  }
  if (unixListener.joinable()) {
    unixListener.join();
  }
  if (healthCheckThread.joinable()) {
    healthCheckThread.join();
  }
  if (metricsSnapshotThread_.joinable()) {
    metricsSnapshotThread_.join();
  }

  bus_.stop();

  // Clean up UNIX socket file
  unlink(unixSocketPath.c_str());

  logger::info("ServiceGateway stopped");
}

void ServiceGateway::startTcpListener() {
  const int serverFd = createTcpSocket();
  if (serverFd == -1) {
    logger::error("Failed to create TCP socket");
    return;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(tcpPort);

  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    logger::error("TCP bind failed", "port=" + std::to_string(tcpPort));
    close(serverFd);
    return;
  }

  if (listen(serverFd, 10) < 0) {
    logger::error("TCP listen failed");
    close(serverFd);
    return;
  }

  logger::info("TCP listener started", "port=" + std::to_string(tcpPort));
  tcpServerFd = serverFd;

  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serverFd, &readfds);
    struct timeval tv{.tv_sec = 1, .tv_usec = 0}; // 1-second timeout so stop() unblocks quickly
    if (select(serverFd + 1, &readfds, nullptr, nullptr, &tv) <= 0) {
      continue;
    }

    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    const int clientFd =
        accept(serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
    if (clientFd < 0) {
      if (running.load()) {
        logger::error("TCP accept failed");
      }
      continue;
    }

    // Get client IP
    // char clientIP[INET_ADDRSTRLEN];
    std::array<char, INET_ADDRSTRLEN> clientIP;
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP.data(), INET_ADDRSTRLEN);

    const std::string tcpTarget =
        std::string(clientIP.data()) + ":" + std::to_string(ntohs(clientAddr.sin_port));
    handleNewConnection(clientFd, tcpTarget, "tcp");
  }

  tcpServerFd = -1;
  close(serverFd);
}

void ServiceGateway::startUnixListener() {
  const int serverFd = createUnixSocket();
  if (serverFd == -1) {
    logger::error("Failed to create UNIX socket");
    return;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  strncpy(address.sun_path, unixSocketPath.c_str(), sizeof(address.sun_path) - 1);

  // Remove existing socket file
  unlink(unixSocketPath.c_str());

  if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    logger::error("UNIX bind failed", unixSocketPath);
    close(serverFd);
    return;
  }

  if (listen(serverFd, 10) < 0) {
    logger::error("UNIX listen failed");
    close(serverFd);
    return;
  }

  logger::info("UNIX listener started", unixSocketPath);
  unixServerFd = serverFd;

  while (running.load()) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serverFd, &readfds);
    struct timeval tv{.tv_sec = 1, .tv_usec = 0};
    if (select(serverFd + 1, &readfds, nullptr, nullptr, &tv) <= 0) {
      continue;
    }

    const int clientFd = accept(serverFd, nullptr, nullptr);
    if (clientFd < 0) {
      if (running.load()) {
        logger::error("UNIX accept failed");
      }
      continue;
    }

    handleNewConnection(clientFd, unixSocketPath, "unix");
  }

  unixServerFd = -1;
  close(serverFd);
}

void ServiceGateway::startHealthChecker() {
  while (running.load()) {
    // Sleep in short increments so stop() doesn't wait 30 seconds.
    for (int i = 0; i < 30 && running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (running.load()) {
      performHealthCheck();
      cleanupExpiredRequests();
    }
  }
}

void ServiceGateway::handleNewConnection(const int clientFd, const std::string& address,
                                         const std::string& type) {
  std::scoped_lock lock(connectionsMutex);

  activeConnections[clientFd] = {.socketFd = clientFd, .address = address, .connectionType = type};

  logger::info("New connection", type + " from " + address + " fd=" + std::to_string(clientFd));

  // Start listening for messages from this client
  std::thread([this, clientFd]() {
    std::array<char, 4096> buffer;
    std::string accumulator;
    while (running.load()) {
      const ssize_t bytesRead = recv(clientFd, buffer.data(), sizeof(buffer) - 1, 0);
      if (bytesRead <= 0) {
        closeConnection(clientFd);
        break;
      }
      buffer[bytesRead] = '\0';
      accumulator.append(buffer.data(), bytesRead);

      // Split on newline — each complete JSON message ends with '\n'
      std::string::size_type pos;
      while ((pos = accumulator.find('\n')) != std::string::npos) {
        std::string message = accumulator.substr(0, pos);
        accumulator.erase(0, pos + 1);
        if (!message.empty()) {
          handleClientMessage(clientFd, message);
        }
      }
    }
  }).detach();
}

void ServiceGateway::handleClientMessage(const int clientFd, const std::string& message) {
  try {
    rapidjson::Document jsonMessage;
    jsonMessage.Parse(message.c_str());

    if (jsonMessage.HasParseError() || !jsonMessage.IsObject()) {
      logger::error("Invalid JSON from client", "fd=" + std::to_string(clientFd) + " raw=" + message.substr(0, 120));
      return;
    }

    const auto messageType = json::getString(jsonMessage, "type");
    if (!messageType.has_value()) {
      logger::error("Missing message type from client", "fd=" + std::to_string(clientFd));
      return;
    }

    if (messageType.value() == "IDENTIFY") {
      handleIdentifyMessage(clientFd, jsonMessage);
    } else if (messageType.value() == "PING") {
      handlePingMessage(clientFd, jsonMessage);
    } else if (messageType.value() == "RESPONSE") {
      handleResponseMessage(clientFd, jsonMessage);
    } else {
      logger::warn("Unknown message type", messageType.value());
    }
  } catch (const std::exception& e) {
    logger::error("Error handling message", "fd=" + std::to_string(clientFd) + " " + e.what());
  }
}

bool ServiceGateway::handleIdentifyMessage(const int clientFd, const rapidjson::Document& message) {
  try {
    const auto* identityObj = json::getObject(message, "identity");
    if (identityObj == nullptr) {
      logger::error("Invalid identification: missing identity object");
      return false;
    }

    // Parse service identity from message
    ServiceIdentity identity = ServiceIdentity::fromJson(*identityObj);

    // Validate required fields
    if (identity.serviceId.empty() || identity.serviceName.empty()) {
      logger::error("Invalid identification: missing serviceId or serviceName");
      return false;
    }

    // Set connection info
    {
      std::scoped_lock lock(connectionsMutex);
      auto it = activeConnections.find(clientFd);
      if (it != activeConnections.end()) {
        identity.connectionType = it->second.connectionType;
        identity.clientAddress = it->second.address;
        it->second.serviceId = identity.serviceId;
        it->second.identified = true;
      }
    }

    // Register in service registry
    if (registry.registerService(identity)) {
      // Send acknowledgment
      rapidjson::Document ackMessage;
      ackMessage.SetObject();
      auto& allocator = ackMessage.GetAllocator();
      rapidjson::Value ackValue = json::JsonObj(allocator)
                                       .set("type", "ACKNOWLEDGED")
                                       .set("serviceId", identity.serviceId)
                                       .set("status", "registered")
                                       .take();
      ackValue.Swap(ackMessage);

      sendMessage(clientFd, json::docToString(ackMessage));

      logger::info("service_connected", identity.serviceId + " (" + identity.serviceName + ") from " + identity.clientAddress);

      // Publish lifecycle event
      {
        rapidjson::Document ev;
        ev.SetObject();
        auto& a = ev.GetAllocator();
        rapidjson::Value evValue = json::JsonObj(a)
            .set("serviceId", identity.serviceId)
            .set("serviceName", identity.serviceName)
            .set("address", identity.clientAddress)
            .take();
        evValue.Swap(ev);

        bus_.publish("service.connected", ev);
      }

      return true;
    } else {
      logger::error("Service registration failed", identity.serviceId);
      return false;
    }
  } catch (const std::exception& e) {
    logger::error("Error in handleIdentifyMessage", e.what());
    return false;
  }
}

bool ServiceGateway::handlePingMessage(const int clientFd, const rapidjson::Document& message) {
  std::string serviceId;

  // Find serviceId for this connection
  {
    std::scoped_lock lock(connectionsMutex);
    if (const auto it = activeConnections.find(clientFd);
        it != activeConnections.end() && it->second.identified) {
      serviceId = it->second.serviceId;
    }
  }

  if (!serviceId.empty()) {
    // Update stats if provided
    if (const auto* stats = json::getObject(message, "stats")) {
      if (const auto currentLoad = json::getUInt(*stats, "currentLoad")) {
        registry.updateCurrentLoad(serviceId, currentLoad.value());
      }
    }

    // Update ping time
    registry.pingService(serviceId);

    // Send pong
    rapidjson::Document pongMessage;
    pongMessage.SetObject();
    auto& allocator = pongMessage.GetAllocator();
    rapidjson::Value pongValue = json::JsonObj(allocator)
        .set("type", "PONG")
        .set("timestamp",
             static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count()))
        .take();
    pongValue.Swap(pongMessage);
    return sendMessage(clientFd, json::docToString(pongMessage));
  }

  return false;
}

bool ServiceGateway::handleResponseMessage(const int clientFd, const rapidjson::Document& message) {

  const auto requestId = json::getString(message, "requestId");
  if (!requestId.has_value()) {
    logger::error("Received RESPONSE without valid requestId", "fd=" + std::to_string(clientFd));
    return false;
  }

  const auto now = std::chrono::steady_clock::now();

  std::scoped_lock lock(requestsMutex);
  const auto it = pendingRequests.find(requestId.value());
  if (it == pendingRequests.end()) {
    logger::warn("Received response for unknown requestId", requestId.value() + " fd=" + std::to_string(clientFd));
    return false;
  }

  PendingRequest& pending = it->second;
  pending.updatedAt = now;

  if (message.HasMember("data")) {
    pending.responsePayload = json::docToString(message["data"]);
  }

  bool isError = false;
  std::string errorMessage = json::getString(message, "error").value_or("");

  if (const auto* dataObj = json::getObject(message, "data")) {
    const auto& data = *dataObj;
    const auto status = json::getString(data, "status");
    if (status.has_value() && status.value() == "error") {
      isError = true;
    }

    const auto dataError = json::getString(data, "error");
    if (dataError.has_value()) {
      isError = true;
      errorMessage = dataError.value();
    }
    const auto dataMessage = json::getString(data, "message");
    if (isError && errorMessage.empty() && dataMessage.has_value()) {
      errorMessage = dataMessage.value();
    }
  }

  const auto latencyMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - pending.createdAt);
  registry.recordResponseTime(pending.targetServiceId, latencyMs);

  if (isError) {
    pending.state = RequestState::FAILED;
    // Propagate statusCode from payload if available
    if (const auto* dataObj = json::getObject(message, "data")) {
      const auto statusCode = json::getInt(*dataObj, "statusCode");
      if (statusCode.has_value()) {
        pending.statusCode = statusCode.value();
      } else {
        pending.statusCode = 500;
      }
      pending.errorMessage =
          errorMessage.empty() ? "Service returned an error response" : errorMessage;
      registry.recordServiceError(pending.targetServiceId);
    }
  } else {
    pending.state = RequestState::COMPLETED;
    pending.statusCode = 200;
    pending.errorMessage.clear();
  }

  logger::info("Response received", "requestId=" + requestId.value() + " fd=" + std::to_string(clientFd) + " state=" + requestStateToString(pending.state));

  // Notify any HTTP thread waiting on this requestId
  auto waiterIt = responseWaiters_.find(requestId.value());
  if (waiterIt != responseWaiters_.end()) {
    waiterIt->second->notify_one();
  }

  logger::info("response_correlated", requestId.value() + " service=" + pending.targetServiceId + " state=" + requestStateToString(pending.state));

  // Publish request lifecycle event (bus_.publish is non-blocking)
  {
    rapidjson::Document ev;
    ev.SetObject();
    auto& a = ev.GetAllocator();

    rapidjson::Value evValue = json::JsonObj(a)
        .set("requestId", requestId.value())
        .set("capability", pending.capability)
        .set("serviceId", pending.targetServiceId)
        .set("success", !isError)
        .set("latencyMs", static_cast<int>(latencyMs.count()))
        .take();

    evValue.Swap(ev);
    bus_.publish("request.completed", ev);
  }

  return true;
}

PendingRequest ServiceGateway::waitForResponse(const std::string& requestId,
                                               const std::chrono::milliseconds timeout) {
  auto cv = std::make_shared<std::condition_variable>();

  std::unique_lock lock(requestsMutex);
  responseWaiters_[requestId] = cv;

  const bool completed = cv->wait_for(lock, timeout, [&]() {
    const auto it = pendingRequests.find(requestId);
    if (it == pendingRequests.end()) {
      return true;
    }
    return it->second.state == RequestState::COMPLETED ||
           it->second.state == RequestState::FAILED || it->second.state == RequestState::TIMED_OUT;
  });

  responseWaiters_.erase(requestId);

  const auto it = pendingRequests.find(requestId);
  if (it == pendingRequests.end()) {
    PendingRequest notFound;
    notFound.requestId = requestId;
    notFound.state = RequestState::FAILED;
    notFound.statusCode = 404;
    notFound.errorMessage = "Request not found";
    return notFound;
  }

  PendingRequest result = it->second;
  if (!completed) {
    result.state = RequestState::TIMED_OUT;
    result.statusCode = 504;
    result.errorMessage = "Request timed out waiting for service response";
  }

  pendingRequests.erase(it);
  return result;
}

std::string ServiceGateway::sendRequest(const std::string& capability,
                                        const rapidjson::Document& requestData,
                                        const LoadBalancingStrategy strategy) {
  // Apply routing rules: may remap the incoming capability to another.
  const std::string resolvedCapability = router_.resolve(capability, requestData);

  // Use per-capability LB strategy from config, unless caller passed an explicit one
  // (caller default is LEAST_LOADED — treat that as "use config").
  const LoadBalancingStrategy effectiveStrategy = (strategy == LoadBalancingStrategy::LEAST_LOADED)
                                                      ? config_.lbStrategyFor(resolvedCapability)
                                                      : strategy;

  const std::string targetServiceId =
      registry.selectBestService(resolvedCapability, effectiveStrategy);
  if (targetServiceId.empty()) {
    logger::warn("No available service for capability", resolvedCapability);
    return "";
  }

  std::string requestId = generateRequestId();

  PendingRequest pending;
  pending.requestId = requestId;
  pending.targetServiceId = targetServiceId;
  pending.capability = resolvedCapability;
  pending.requestPayload = json::docToString(requestData);
  pending.state = RequestState::QUEUED;
  pending.statusCode = 202;
  pending.createdAt = std::chrono::steady_clock::now();
  pending.updatedAt = pending.createdAt;

  {
    std::scoped_lock lock(requestsMutex);
    pendingRequests[requestId] = std::move(pending);
  }

  rapidjson::Document requestMessage;
  requestMessage.SetObject();
  auto& allocator = requestMessage.GetAllocator();

  rapidjson::Value data;
  data.CopyFrom(requestData, allocator);

  rapidjson::Value requestMessageValue = json::JsonObj(allocator)
      .set("type", "REQUEST")
      .set("requestId", requestId)
      .set("capability", resolvedCapability)
      .setValue("data", std::move(data))
      .take();

  requestMessage.Swap(requestMessageValue);

  if (!sendDirectRequest(targetServiceId, requestMessage)) {
    std::scoped_lock lock(requestsMutex);
    pendingRequests.erase(requestId);
    return "";
  }

  {
    std::scoped_lock lock(requestsMutex);
    auto it = pendingRequests.find(requestId);
    if (it != pendingRequests.end() && it->second.state == RequestState::QUEUED) {
      it->second.state = RequestState::IN_FLIGHT;
      it->second.updatedAt = std::chrono::steady_clock::now();
    }
  }

  return requestId;
}

bool ServiceGateway::sendDirectRequest(const std::string& serviceId,
                                       const rapidjson::Document& requestData) {
  int targetFd = -1;
  {
    std::scoped_lock lock(connectionsMutex);
    for (const auto& [fd, conn] : activeConnections) {
      if (conn.identified && conn.serviceId == serviceId) {
        targetFd = fd;
        break;
      }
    }
  }

  if (targetFd == -1) {
    logger::error("Service not connected", serviceId);
    return false;
  }

  rapidjson::Document outbound;
  outbound.CopyFrom(requestData, outbound.GetAllocator());

  return sendMessage(targetFd, json::docToString(outbound));
}

void ServiceGateway::closeConnection(const int clientFd) {
  std::string disconnectedServiceId;

  {
    std::scoped_lock lock(connectionsMutex);

    if (const auto it = activeConnections.find(clientFd); it != activeConnections.end()) {
      logger::info("Closing connection", "fd=" + std::to_string(clientFd) + " addr=" + it->second.address);

      if (it->second.identified && !it->second.serviceId.empty()) {
        disconnectedServiceId = it->second.serviceId;
        logger::info("service_disconnected", it->second.serviceId + " connection closed");
        registry.unregisterService(it->second.serviceId);
      }
    }
  }

  // Publish disconnect event outside the mutex
  if (!disconnectedServiceId.empty()) {
    rapidjson::Document ev;
    ev.SetObject();
    auto& a = ev.GetAllocator();

    rapidjson::Value evValue = json::JsonObj(a)
        .set("serviceId", disconnectedServiceId)
        .take();
    evValue.Swap(ev);
    bus_.publish("service.disconnected", ev);
  }

  // Fail any in-flight requests that were routed to the disconnected service
  if (!disconnectedServiceId.empty()) {
    std::scoped_lock lock(requestsMutex);
    for (auto& [reqId, pending] : pendingRequests) {
      if (pending.targetServiceId == disconnectedServiceId &&
          (pending.state == RequestState::QUEUED || pending.state == RequestState::IN_FLIGHT)) {
        pending.state = RequestState::FAILED;
        pending.statusCode = 503;
        pending.errorMessage = "Service disconnected";

        if (const auto waiterIt = responseWaiters_.find(reqId);
            waiterIt != responseWaiters_.end()) {
          waiterIt->second->notify_one();
        }
      }
    }
  }
}

rapidjson::Document ServiceGateway::getGatewayStatus() const {
  rapidjson::Document status;
  status.SetObject();
  auto& allocator = status.GetAllocator();

  rapidjson::Document registryStatus = registry.getRegistryStatus();
  rapidjson::Value registryValue;
  registryValue.CopyFrom(registryStatus, allocator);

  rapidjson::Value statusValue = json::JsonObj(allocator)
      .set("status", running.load())
      .set("tcpPort", tcpPort)
      .set("unixSocket", unixSocketPath)
      .set("activeConnections", static_cast<int>(getActiveConnectionCount()))
      .set("trackedRequests", static_cast<int>(getTrackedRequestCount()))
      .setValue("registryStatus", std::move(registryValue))
      .take();
  status.Swap(statusValue);
  return status;
}

rapidjson::Document ServiceGateway::getConnectionStatus() const {
  std::scoped_lock lock(connectionsMutex);

  rapidjson::Document connections;
  connections.SetArray();
  auto& allocator = connections.GetAllocator();
  for (const auto& [fd, conn] : activeConnections) {

    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - conn.connectedAt)
                            .count();


    rapidjson::Value connInfoValue = json::JsonObj(allocator)
        .set("fd", fd)
        .set("address", conn.address)
        .set("type", conn.connectionType)
        .set("serviceId", conn.serviceId)
        .set("identified", conn.identified)
        .set("uptimeSeconds", static_cast<int64_t>(uptime))
        .take();

    connections.PushBack(connInfoValue, allocator);
  }

  return connections;
}

size_t ServiceGateway::getActiveConnectionCount() const {
  std::scoped_lock lock(connectionsMutex);
  return activeConnections.size();
}

size_t ServiceGateway::getTrackedRequestCount() const {
  std::scoped_lock lock(requestsMutex);
  return pendingRequests.size();
}

void ServiceGateway::recordMetric(const std::string& capability, const double latencyMs,
                                  const bool success, const bool timedOut) {
  metrics_.record(capability, latencyMs, success, timedOut);
}

rapidjson::Document ServiceGateway::getMetrics() const {
  return metrics_.toJson();
}

rapidjson::Document ServiceGateway::getHealth() const {
  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();

  const auto uptimeSec = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();



  // Services
  rapidjson::Document registryDoc = registry.getRegistryStatus();
  rapidjson::Value services(rapidjson::kArrayType);

  if (const auto* registryServices = json::getArray(registryDoc, "services")) {
    // Merge registry info with per-capability metrics
    rapidjson::Document metricsDoc = metrics_.toJson();

    // Build a quick lookup: capability -> avgLatencyMs, errorRate
    std::map<std::string, const rapidjson::Value*> capMetrics;
    if (const auto* capabilities = json::getArray(metricsDoc, "capabilities")) {
      for (const auto& entry : capabilities->GetArray()) {
        const auto cap = json::getString(entry, "capability");
        if (cap.has_value()) {
          capMetrics[cap.value()] = &entry;
        }
      }
    }

    for (const auto& svc : registryServices->GetArray()) {
      json::JsonObj entry(alloc);

      // Copy core identity fields
      for (const auto& m : {"serviceId", "serviceName", "machineName", "version"}) {
        if (svc.HasMember(m)) {
          rapidjson::Value val;
          val.CopyFrom(svc[m], alloc);
          entry.setValue(m, std::move(val));
        }
      }

      // Derive per-service aggregate metrics from its capabilities
      double totalAvg = 0.0;
      double totalErrorRate = 0.0;
      int capCount = 0;

      if (const auto* svcCapabilities = json::getArray(svc, "capabilities")) {
        rapidjson::Value caps;
        caps.CopyFrom(*svcCapabilities, alloc);
        entry.setValue("capabilities", std::move(caps));

        for (const auto& capVal : svcCapabilities->GetArray()) {
          const std::string cap = capVal.GetString();
          if (capMetrics.contains(cap)) {
            const auto* cm = capMetrics.at(cap);
            if (cm->HasMember("avgLatencyMs")) {
              totalAvg += (*cm)["avgLatencyMs"].GetDouble();
            }
            if (cm->HasMember("errorRate")) {
              totalErrorRate += (*cm)["errorRate"].GetDouble();
            }
            capCount++;
          }
        }
      }

      if (capCount > 0) {
        entry.set("avgLatencyMs", std::round(totalAvg / capCount * 100.0) / 100.0);
        entry.set("errorRate", std::round(totalErrorRate / capCount * 10000.0) / 10000.0);
      }

      services.PushBack(entry.take(), alloc);
    }
  }

  // Gateway config
  rapidjson::Value gw = json::JsonObj(alloc)
        .set("tcpPort", tcpPort)
        .set("unixSocket", unixSocketPath)
        .set("activeConnections", static_cast<int>(getActiveConnectionCount()))
        .set("pendingRequests", static_cast<int>(getTrackedRequestCount()))
        .take();

  rapidjson::Value docValue = json::JsonObj(alloc)
      .set("status", running.load() ? "healthy" : "stopped")
      .set("uptimeEpochSec", static_cast<int64_t>(uptimeSec))
      .setValue("gateway", std::move(gw))
      .setValue("services", std::move(services))
      .take();

  docValue.Swap(doc);
  return doc;
}

rapidjson::Document ServiceGateway::getRequestStatus(const std::string& requestId) const {
  rapidjson::Document response;
  response.SetObject();
  auto& allocator = response.GetAllocator();


  std::scoped_lock lock(requestsMutex);
  const auto it = pendingRequests.find(requestId);
  if (it == pendingRequests.end()) {
    rapidjson::Value notFoundValue = json::JsonObj(allocator)
        .set("requestId", requestId)
        .set("found", false)
        .set("status", "not_found")
        .set("message", "Request ID not found")
        .take();
    notFoundValue.Swap(response);
    return response;
  }

  const PendingRequest& pending = it->second;
  const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - pending.createdAt)
                         .count();

  json::JsonObj requestInfo(allocator);
  requestInfo.set("requestId", pending.requestId)
      .set("found", true)
      .set("status", requestStateToString(pending.state))
      .set("statusCode", pending.statusCode)
      .set("targetServiceId", pending.targetServiceId)
      .set("capability", pending.capability)
      .set("ageMs", static_cast<int64_t>(ageMs))
      .set("timeoutMs", static_cast<int64_t>(pending.timeout.count()));

  if (!pending.responsePayload.empty()) {
    requestInfo.set("responsePayload", pending.responsePayload);
  } else {
    requestInfo.setValue("responsePayload", rapidjson::Value(rapidjson::kNullType));
  }

  if (!pending.errorMessage.empty()) {
    requestInfo.set("errorMessage", pending.errorMessage);
  } else {
    requestInfo.setValue("errorMessage", rapidjson::Value(rapidjson::kNullType));
  }

  rapidjson::Value requestInfoValue = requestInfo.take();

  requestInfoValue.Swap(response);

  return response;
}

void ServiceGateway::setMessageHandler(const std::string& messageType, MessageHandler handler) {
  messageHandlers[messageType] = std::move(handler);
}

// Helper methods
bool ServiceGateway::sendMessage(const int socketFd, const std::string& message) {
  const std::string framed = message + "\n";
  const ssize_t sent = send(socketFd, framed.c_str(), framed.length(), MSG_NOSIGNAL);
  return std::cmp_equal(sent, framed.length());
}

std::string ServiceGateway::requestStateToString(const RequestState state) {
  switch (state) {
    case RequestState::QUEUED:
      return "queued";
    case RequestState::IN_FLIGHT:
      return "in_flight";
    case RequestState::COMPLETED:
      return "completed";
    case RequestState::FAILED:
      return "failed";
    case RequestState::TIMED_OUT:
      return "timed_out";
  }

  return "unknown";
}

std::string ServiceGateway::generateRequestId() {
  return "req_" + std::to_string(requestIdCounter.fetch_add(1));
}

void ServiceGateway::cleanupExpiredRequests() {
  constexpr auto retention = std::chrono::minutes(5);
  const auto now = std::chrono::steady_clock::now();

  std::scoped_lock lock(requestsMutex);
  for (auto it = pendingRequests.begin(); it != pendingRequests.end();) {
    PendingRequest& pending = it->second;
    const auto age = now - pending.createdAt;

    if ((pending.state == RequestState::QUEUED || pending.state == RequestState::IN_FLIGHT) &&
        age > pending.timeout) {
      pending.state = RequestState::TIMED_OUT;
      pending.statusCode = 504;
      pending.errorMessage = "Request timed out waiting for service response";
      pending.updatedAt = now;
    }

    const bool isTerminal = pending.state == RequestState::COMPLETED ||
                            pending.state == RequestState::FAILED ||
                            pending.state == RequestState::TIMED_OUT;

    if (isTerminal && (now - pending.updatedAt) > retention) {
      it = pendingRequests.erase(it);
      continue;
    }

    ++it;
  }
}

void ServiceGateway::performHealthCheck() {
  // Remove unhealthy services
  registry.removeUnhealthyServices(std::chrono::seconds(60));
}

int ServiceGateway::createTcpSocket() {
  const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd == -1) {
    return -1;
  }

  // Set socket options
  constexpr int opt = 1;
  setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  return socketFd;
}

int ServiceGateway::createUnixSocket() {
  return socket(AF_UNIX, SOCK_STREAM, 0);
}

} // namespace servicegateway
