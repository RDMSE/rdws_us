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

InvokeResult ServiceClient::invoke(const std::string& capability, const rapidjson::Document& data,
                                   const std::chrono::milliseconds timeout) {
  if (!connected.load()) {
    return InvokeResult{.success = false, .statusCode = 0, .errorMessage = "Not connected to broker"};
  }

  const std::string requestId =
      identity.serviceId + "-invoke-" + std::to_string(invokeCounter.fetch_add(1));

  auto pending = std::make_shared<PendingInvoke>();
  {
    std::scoped_lock lock(invokeMutex);
    pendingInvokes[requestId] = pending;
  }

  rapidjson::Document invokeMessage;
  invokeMessage.SetObject();
  auto& allocator = invokeMessage.GetAllocator();

  // The target's own dispatcher (CapabilityRouter) reads "capability" from inside
  // the payload itself, not from the wire envelope — HttpGateway does the same
  // manual injection for regular HTTP-triggered requests (see the comment in
  // ServiceGateway::start() bridging request.completed → persistence.save.request).
  // Without this, the callee sees an empty capability and replies 404 "Unknown
  // capability: ".
  rapidjson::Value dataValue;
  dataValue.CopyFrom(data, allocator);
  if (dataValue.IsObject()) {
    if (dataValue.HasMember("capability")) {
      dataValue.RemoveMember("capability");
    }
    dataValue.AddMember("capability", rapidjson::Value(capability.c_str(), allocator), allocator);
  }

  rapidjson::Value invokeMessageValue = json::JsonObj(allocator)
      .set("type", "INVOKE")
      .set("requestId", requestId)
      .set("capability", capability)
      .setValue("data", std::move(dataValue))
      .take();
  invokeMessage.Swap(invokeMessageValue);

  if (!sendMessage(invokeMessage)) {
    std::scoped_lock lock(invokeMutex);
    pendingInvokes.erase(requestId);
    return InvokeResult{.success = false, .statusCode = 0, .errorMessage = "Failed to send invoke request"};
  }

  std::unique_lock lock(invokeMutex);
  const bool arrived = invokeCv.wait_for(lock, timeout, [&]() { return pending->done; });
  pendingInvokes.erase(requestId);

  if (!arrived) {
    return InvokeResult{.success = false, .statusCode = 504, .errorMessage = "Timed out waiting for response"};
  }

  return pending->result;
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
  joinRequestThreads();
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

  // Request handlers now run on their own thread (see handleRequest) and may call
  // sendResponse()/invoke() concurrently with each other and with the message loop
  // — serialize the actual socket write so two messages can't interleave.
  std::scoped_lock lock(sendMutex_);
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
    } else if (messageType.value() == "INVOKE_RESPONSE") {
      handleInvokeResponse(jsonMessage);
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

  // Run the handler on its own thread rather than inline on the message loop: if
  // the handler itself calls invoke() to reach another service (see
  // FieldServiceClient), it blocks waiting for an INVOKE_RESPONSE that arrives on
  // this very socket — if messageLoop() were stuck inside requestHandler() it would
  // never get back to recv() to read that response, deadlocking until invoke()'s
  // own timeout gives up. Running the handler elsewhere keeps messageLoop() free to
  // keep servicing the socket.
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto worker = [this, requestId = requestId.value(), requestData = std::move(requestData), done]() {
    try {
      rapidjson::Document response = requestHandler(requestData);
      (void)sendResponse(requestId, response);

      identity.totalRequests++;
      identity.currentLoad = std::max(0, static_cast<int>(identity.currentLoad) - 1);

    } catch (const std::exception& e) {
      logger::error("Error processing request", requestId + ": " + e.what());

      rapidjson::Document errorResponse;
      errorResponse.SetObject();
      errorResponse.AddMember("error", rapidjson::Value(e.what(), errorResponse.GetAllocator()),
                              errorResponse.GetAllocator());
      (void)sendResponse(requestId, errorResponse);

      identity.errorCount++;
    }
    done->store(true);
  };

  std::scoped_lock lock(requestThreadsMutex_);
  // Reap finished workers so the vector doesn't grow unbounded across the client's
  // lifetime.
  for (auto& w : requestThreads_) {
    if (w.done->load() && w.thread.joinable()) {
      w.thread.join();
    }
  }
  std::erase_if(requestThreads_, [](const RequestWorker& w) { return w.done->load(); });

  requestThreads_.push_back(RequestWorker{std::thread(std::move(worker)), done});
}

void ServiceClient::joinRequestThreads() {
  std::scoped_lock lock(requestThreadsMutex_);
  for (auto& w : requestThreads_) {
    if (w.thread.joinable()) {
      w.thread.join();
    }
  }
  requestThreads_.clear();
}

void ServiceClient::handleInvokeResponse(const rapidjson::Document& message) {
  const auto requestId = json::getString(message, "requestId");
  if (!requestId.has_value()) {
    logger::error("Received INVOKE_RESPONSE without requestId");
    return;
  }

  std::shared_ptr<PendingInvoke> pending;
  {
    std::scoped_lock lock(invokeMutex);
    const auto it = pendingInvokes.find(requestId.value());
    if (it == pendingInvokes.end()) {
      // Already timed out locally and stopped waiting — nothing to deliver to.
      return;
    }
    pending = it->second;
  }

  pending->result.success = json::getBool(message, "success").value_or(false);
  pending->result.statusCode = json::getInt(message, "statusCode").value_or(0);
  if (pending->result.success) {
    if (const auto* dataObj = json::getObject(message, "data")) {
      pending->result.responsePayload = json::docToString(*dataObj);
    }
  } else {
    pending->result.errorMessage = json::getString(message, "error").value_or("Invoke failed");
  }

  {
    std::scoped_lock lock(invokeMutex);
    pending->done = true;
  }
  invokeCv.notify_all();
}

void ServiceClient::handlePong(const rapidjson::Document& message) {
  logger::info("Received PONG from broker");
}

} // namespace servicegateway
