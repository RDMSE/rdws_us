#pragma once

#include "ServiceRegistry.h"
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>

namespace servicebroker {

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
    std::string clientData;
    std::chrono::time_point<std::chrono::steady_clock> timestamp = std::chrono::steady_clock::now();
};

using MessageHandler = std::function<void(const std::string& serviceId, const Json::Value& message)>;

class ServiceBroker {
private:
    // Configuration
    int tcpPort = 8080;
    std::string unixSocketPath = "/tmp/service_broker.sock";
    
    // Core components
    ServiceRegistry registry;
    
    // Network threads
    std::thread tcpListener;
    std::thread unixListener;
    std::atomic<bool> running{false};
    
    // Connection management
    std::map<int, ClientConnection> activeConnections;
    mutable std::mutex connectionsMutex;
    
    // Request management
    std::map<std::string, PendingRequest> pendingRequests;
    std::mutex requestsMutex;
    std::atomic<uint64_t> requestIdCounter{0};
    
    // Message handlers
    std::map<std::string, MessageHandler> messageHandlers;
    
    // Health check thread
    std::thread healthCheckThread;
    
public:
    explicit ServiceBroker(int port = 8080, std::string  unixSocket = "/tmp/service_broker.sock");
    ~ServiceBroker();
    
    // Lifecycle management
    bool start();
    void stop();
    bool isRunning() const { return running.load(); }
    
    // Network listeners
    void startTcpListener();
    void startUnixListener();
    void startHealthChecker();
    
    // Connection management
    void handleNewConnection(int clientFd, const std::string& address, const std::string& type);
    void handleClientMessage(int clientFd, const std::string& message);
    void closeConnection(int clientFd);
    
    // Protocol handlers
    bool handleIdentifyMessage(int clientFd, const Json::Value& message);
    bool handlePingMessage(int clientFd, const Json::Value& message);
    static bool handleResponseMessage(int clientFd, const Json::Value& message);
    
    // Request routing
    std::string sendRequest(const std::string& capability, const Json::Value& requestData, 
                          LoadBalancingStrategy strategy = LoadBalancingStrategy::LEAST_LOADED);
    bool sendDirectRequest(const std::string& serviceId, const Json::Value& requestData);
    
    // Monitoring and management
    Json::Value getBrokerStatus() const;
    Json::Value getConnectionStatus() const;
    size_t getActiveConnectionCount() const;
    
    // Message handlers registration
    void setMessageHandler(const std::string& messageType, MessageHandler handler);
    
    // Registry access
    ServiceRegistry& getRegistry() { return registry; }
    const ServiceRegistry& getRegistry() const { return registry; }
    
private:
    // Helper methods
    static bool sendMessage(int socketFd, const std::string& message);
    std::string generateRequestId();

    static void cleanupExpiredRequests();
    void performHealthCheck();
    
    // Socket helpers
    static int createTcpSocket();
    static int createUnixSocket();
    void setSocketNonBlocking(int socketFd);
};

} // namespace servicebroker