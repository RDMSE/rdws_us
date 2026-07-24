#pragma once

#include "ServiceIdentity.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

namespace servicegateway {

using RequestHandler = std::function<rapidjson::Document(const rapidjson::Document& request)>;

// Result of a synchronous cross-service call made via ServiceClient::invoke().
struct InvokeResult {
  bool success = false;
  int statusCode = 0;
  std::string responsePayload; // raw JSON of the "data" field on success
  std::string errorMessage;    // set when success is false
};

class ServiceClient {
private:
  ServiceIdentity identity;
  std::string gatewayAddress; // "tcp://localhost:8080" or "unix:///tmp/service_gateway.sock"
  int socketFd = -1;

  std::atomic<bool> connected{false};
  std::atomic<bool> registered{false};
  std::atomic<bool> stopRequested{false};
  std::thread messageThread;
  std::thread pingThread;

  RequestHandler requestHandler;

  // Serializes writes to socketFd — handleRequest() now dispatches the handler onto
  // its own thread (see below), so responses/invokes can be sent concurrently with
  // each other and with the message loop's own writes.
  mutable std::mutex sendMutex_;

  // Threads spawned by handleRequest() to run the request handler off the message
  // loop (see handleRequest for why). `done` flips to true right before the worker
  // returns, so finished entries can be reaped (joined + erased) without blocking.
  struct RequestWorker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<RequestWorker> requestThreads_;
  std::mutex requestThreadsMutex_;

  // Pending outbound invoke() calls awaiting an INVOKE_RESPONSE, keyed by requestId.
  struct PendingInvoke {
    bool done = false;
    InvokeResult result;
  };
  std::atomic<uint64_t> invokeCounter{0};
  std::map<std::string, std::shared_ptr<PendingInvoke>> pendingInvokes;
  std::mutex invokeMutex;
  std::condition_variable invokeCv;

public:
  explicit ServiceClient(ServiceIdentity serviceIdentity,
                         std::string address = "unix:///tmp/service_gateway.sock");
  ~ServiceClient();

  // Connection management
  bool connect();
  void disconnect();
  [[nodiscard]] bool isConnected() const {
    return connected.load();
  }
  [[nodiscard]] bool isRegistered() const {
    return registered.load();
  }

  // Service registration
  [[nodiscard]] bool registerService() const;

  // Request handling
  void setRequestHandler(const RequestHandler& handler);

  // Communication
  [[nodiscard]] bool sendPing() const;
  [[nodiscard]] bool sendPing(const rapidjson::Document& stats) const;
  [[nodiscard]] bool sendResponse(const std::string& requestId,
                                  const rapidjson::Document& response) const;

  // Synchronous call to another service's capability, routed through the gateway
  // (ServiceGateway::handleInvokeMessage) — hides requestId generation, correlation
  // and timeout from the caller. Blocks the calling thread until the response
  // arrives or the timeout elapses.
  [[nodiscard]] InvokeResult invoke(const std::string& capability, const rapidjson::Document& data,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  // Main event loop. Reconnects with backoff on connection loss; keeps retrying
  // until stop() is called.
  void run();
  void stop();

private:
  // Socket management
  [[nodiscard]] int createConnection() const;
  [[nodiscard]] bool sendMessage(const rapidjson::Document& message) const;
  void messageLoop();
  void pingLoop() const;
  // Blocks in small increments (so stop() can interrupt promptly) for `backoff`,
  // then doubles it up to a cap. Returns the next backoff to use.
  std::chrono::milliseconds waitBeforeReconnect(std::chrono::milliseconds backoff) const;

  // Message handlers
  void handleMessage(const std::string& message);
  void handleRequest(const rapidjson::Document& message);
  void handleInvokeResponse(const rapidjson::Document& message);
  static void handlePong(const rapidjson::Document& message);
  void joinRequestThreads();
};

} // namespace servicegateway
