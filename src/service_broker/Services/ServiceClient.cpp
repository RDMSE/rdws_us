#include "ServiceClient.h"

#include "../../shared/utils/json_helper.h"
#include "../../shared/utils/logger.h"
#include "../../shared/utils/response_helper.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace logger = rdws::utils::logger;
namespace json = rdws::utils::json;

namespace servicegateway {

ServiceClient::ServiceClient(ServiceIdentity serviceIdentity, std::string address)
    : identity(std::move(serviceIdentity)), gatewayAddress(std::move(address)) {}

ServiceClient::~ServiceClient() {
  stop();
}

bool ServiceClient::connect() {
  if (connected.load()) {
    return true;
  }

  socketFd = createConnection();
  if (socketFd == -1) {
    logger::error("Failed to create connection", gatewayAddress);
    return false;
  }

  connected.store(true);
  logger::info("Connected to broker", gatewayAddress);

  return true;
}

void ServiceClient::disconnect() {
  if (!connected.load()) {
    return;
  }

  connected.store(false);
  registered.store(false);

  if (socketFd != -1) {
    shutdown(socketFd, SHUT_RDWR); // unblock any blocking recv() in messageLoop
    close(socketFd);
    socketFd = -1;
  }

  logger::info("Disconnected from broker");
}

bool ServiceClient::registerService() const {
  if (!connected.load()) {
    logger::error("Not connected to broker");
    return false;
  }

  rapidjson::Document identifyMessage;
  identifyMessage.SetObject();
  auto& allocator = identifyMessage.GetAllocator();

  rapidjson::Value identityValue = json::JsonObj(allocator)
      .set("type", "IDENTIFY")
      .setValue("identity", identity.toJsonValue(allocator))
      .take();
  identifyMessage.Swap(identityValue);

  if (sendMessage(identifyMessage)) {
    logger::info("Sent identification for service", identity.serviceId);
    return true;
  }

  return false;
}

void ServiceClient::setRequestHandler(const RequestHandler& handler) {
  requestHandler = handler;
}

bool ServiceClient::sendPing() const {
  const rapidjson::Document empty;
  return sendPing(empty);
}

bool ServiceClient::sendPing(const rapidjson::Document& stats) const {
  if (!connected.load()) {
    return false;
  }

  rapidjson::Document pingMessage;
  pingMessage.SetObject();
  auto& allocator = pingMessage.GetAllocator();

  json::JsonObj pingMessageObj(allocator);
  pingMessageObj.set("type", "PING")
      .set("serviceId", identity.serviceId)
      .set("timestamp",
           static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count()));

  if (!stats.IsNull() && stats.IsObject()) {
    rapidjson::Value statsValue;
    statsValue.CopyFrom(stats, allocator);
    pingMessageObj.setValue("stats", std::move(statsValue));
  }

  rapidjson::Value result = pingMessageObj.take();
  pingMessage.Swap(result);

  return sendMessage(pingMessage);
}

bool ServiceClient::sendResponse(const std::string& requestId,
                                 const rapidjson::Document& response) const {
  if (!connected.load()) {
    return false;
  }

  rapidjson::Document responseMessage;
  responseMessage.SetObject();
  auto& allocator = responseMessage.GetAllocator();

  json::JsonObj responseMessageObj(allocator);
  responseMessageObj.set("type", "RESPONSE")
      .set("requestId", requestId)
      .set("serviceId", identity.serviceId);


  rapidjson::Value data;
  data.CopyFrom(response, allocator);
  responseMessageObj.setValue("data", std::move(data));

  rapidjson::Value responseResult = responseMessageObj.take();
  responseMessage.Swap(responseResult);

  return sendMessage(responseMessage);
}

void ServiceClient::run() {
  if (!connect()) {
    return;
  }

  if (!registerService()) {
    disconnect();
    return;
  }

  // Start message and ping threads
  messageThread = std::thread(&ServiceClient::messageLoop, this);
  pingThread = std::thread(&ServiceClient::pingLoop, this);

  logger::info("Service client running", identity.serviceId);

  // Wait for threads to finish
  if (messageThread.joinable()) {
    messageThread.join();
  }
  if (pingThread.joinable()) {
    pingThread.join();
  }
}

void ServiceClient::stop() {
  disconnect();
  // messageThread and pingThread are owned and joined by run().
  // Callers must join the thread that called run() to ensure full cleanup.
}

int ServiceClient::createConnection() const {
  if (gatewayAddress.starts_with("tcp://")) {
    std::string address = gatewayAddress.substr(6); // Remove "tcp://"
    const size_t colonPos = address.find(':');
    if (colonPos == std::string::npos) {
      logger::error("Invalid TCP address format");
      return -1;
    }

    const std::string host = address.substr(0, colonPos);
    const std::string port = address.substr(colonPos + 1);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* resolved = nullptr;
    const int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved);
    if (rc != 0 || resolved == nullptr) {
      logger::error("Failed to resolve TCP address", host + ":" + port);
      return -1;
    }

    int sockFd = -1;
    for (const addrinfo* rp = resolved; rp != nullptr; rp = rp->ai_next) {
      sockFd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (sockFd == -1) {
        continue;
      }
      if (::connect(sockFd, rp->ai_addr, rp->ai_addrlen) == 0) {
        break;
      }
      close(sockFd);
      sockFd = -1;
    }
    freeaddrinfo(resolved);

    if (sockFd == -1) {
      logger::error("Failed to connect to TCP", host + ":" + port);
      return -1;
    }

    return sockFd;
  }
  if (gatewayAddress.starts_with("unix://")) {
    // UNIX socket connection
    const std::string socketPath = gatewayAddress.substr(7); // Remove "unix://"

    const int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd == -1) {
      logger::error("Failed to create UNIX socket");
      return -1;
    }

    sockaddr_un serverAddr{};
    serverAddr.sun_family = AF_UNIX;
    strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);

    if (::connect(sockFd, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
      logger::error("Failed to connect to UNIX socket", socketPath);
      close(sockFd);
      return -1;
    }

    return sockFd;
  }

  logger::error("Unknown address format", gatewayAddress);
  return -1;
}

bool ServiceClient::sendMessage(const rapidjson::Document& message) const {
  if (socketFd == -1) {
    return false;
  }

  const std::string messageStr = json::docToString(message) + std::string("\n");

  const ssize_t sent = send(socketFd, messageStr.c_str(), messageStr.length(), MSG_NOSIGNAL);
  return std::cmp_equal(sent, messageStr.length());
}

void ServiceClient::messageLoop() {
  char buffer[4096];
  std::string accumulator;

  while (connected.load()) {
    const ssize_t bytesRead = recv(socketFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
      if (connected.load()) {
        logger::error("Connection lost to broker");
      }
      break;
    }

    buffer[bytesRead] = '\0';
    accumulator.append(buffer, bytesRead);

    std::string::size_type pos;
    while ((pos = accumulator.find('\n')) != std::string::npos) {
      std::string message = accumulator.substr(0, pos);
      accumulator.erase(0, pos + 1);
      if (!message.empty()) {
        handleMessage(message);
      }
    }
  }
}

void ServiceClient::pingLoop() const {
  while (connected.load()) {
    // Sleep in 1-second increments so stop()/disconnect() can exit quickly.
    for (int i = 0; i < 30 && connected.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (connected.load() && registered.load()) {
      // Send ping with current stats
      rapidjson::Document stats;
      stats.SetObject();
      auto& allocator = stats.GetAllocator();

      rapidjson::Value statsValue = json::JsonObj(allocator)
        .set("currentLoad", identity.currentLoad)
        .set("totalRequests", identity.totalRequests)
        .set("errorCount", identity.errorCount)
        .take();

      stats.Swap(statsValue);

      (void)sendPing(stats);
    }
  }
}

void ServiceClient::handleMessage(const std::string& message) {
  try {
    rapidjson::Document jsonMessage;
    jsonMessage.Parse(message.c_str());

    if (jsonMessage.HasParseError() || !jsonMessage.IsObject()) {
      logger::error("Invalid JSON from broker");
      return;
    }

    const auto messageType = json::getString(jsonMessage, "type");

    if (!messageType.has_value()) {
      logger::warn("Unknown message without type from broker");
      return;
    }

    if (messageType.value() == "ACKNOWLEDGED") {
      registered.store(true);
      const auto serviceId = json::getString(jsonMessage, "serviceId");
      logger::info("Service registered successfully", serviceId.value_or(""));
    } else if (messageType.value() == "REQUEST") {
      handleRequest(jsonMessage);
    } else if (messageType.value() == "PONG") {
      handlePong(jsonMessage);
    } else {
      logger::warn("Unknown message type from broker", messageType.value());
    }

  } catch (const std::exception& e) {
    logger::error("Error handling broker message", e.what());
  }
}

void ServiceClient::handleRequest(const rapidjson::Document& message) {
  if (!requestHandler) {
    logger::error("No request handler set for service");
    return;
  }

  const auto requestId = json::getString(message, "requestId");

  if (!requestId.has_value() || !message.HasMember("data")) {
    logger::error("Invalid REQUEST message from broker");
    return;
  }

  rapidjson::Document requestData;
  requestData.CopyFrom(message["data"], requestData.GetAllocator());

  logger::info("Processing request", requestId.value());

  try {
    rapidjson::Document response = requestHandler(requestData);

    // Send response back to broker
    (void)sendResponse(requestId.value(), response);

    // Update stats
    identity.totalRequests++;
    identity.currentLoad = std::max(0, static_cast<int>(identity.currentLoad) - 1);

  } catch (const std::exception& e) {
    logger::error("Error processing request", requestId.value() + ": " + e.what());

    // Send error response
    rapidjson::Document errorResponse;
    errorResponse.SetObject();
    errorResponse.AddMember("error", rapidjson::Value(e.what(), errorResponse.GetAllocator()),
                            errorResponse.GetAllocator());
    (void)sendResponse(requestId.value(), errorResponse);

    // Update error count
    identity.errorCount++;
  }
}

void ServiceClient::handlePong(const rapidjson::Document& message) {
  logger::info("Received PONG from broker");
}

} // namespace servicegateway
