#pragma once

#include "ServiceIdentity.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <rapidjson/document.h>

namespace servicegateway {

using RequestHandler = std::function<rapidjson::Document(const rapidjson::Document& request)>;

class ServiceClient {
private:
    ServiceIdentity identity;
    std::string gatewayAddress;  // "tcp://localhost:8080" or "unix:///tmp/service_gateway.sock"
    int socketFd = -1;
    
    std::atomic<bool> connected{false};
    std::atomic<bool> registered{false};
    std::thread messageThread;
    std::thread pingThread;
    
    RequestHandler requestHandler;
    
public:
    explicit ServiceClient(ServiceIdentity  serviceIdentity,
                          const std::string& address = "unix:///tmp/service_gateway.sock");
    ~ServiceClient();
    
    // Connection management
    bool connect();
    void disconnect();
    [[nodiscard]] bool isConnected() const { return connected.load(); }
    [[nodiscard]] bool isRegistered() const { return registered.load(); }
    
    // Service registration
    bool registerService() const;
    
    // Request handling
    void setRequestHandler(const RequestHandler &handler);
    
    // Communication
    bool sendPing();
    bool sendPing(const rapidjson::Document& stats);
    bool sendResponse(const std::string& requestId, const rapidjson::Document& response) const;
    
    // Main event loop
    void run();
    void stop();
    
private:
    // Socket management
    [[nodiscard]] int createConnection() const;
    [[nodiscard]] bool sendMessage(const rapidjson::Document& message) const;
    void messageLoop();
    void pingLoop();
    
    // Message handlers
    void handleMessage(const std::string& message);
    void handleRequest(const rapidjson::Document& message);
    static void handlePong(const rapidjson::Document& message);
};

} // namespace servicegateway