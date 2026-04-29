#include "ServiceBroker.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <ranges>
#include <sstream>
#include <utility>
#include <json/json.h>

namespace servicebroker {

ServiceBroker::ServiceBroker(const int port, std::string  unixSocket)
    : tcpPort(port), unixSocketPath(std::move(unixSocket)) {
    
    // Set up default message handlers
    setMessageHandler("IDENTIFY", [this](const std::string& serviceId, const Json::Value& message) {
        // Already handled in handleIdentifyMessage
    });
    
    setMessageHandler("PING", [this](const std::string& serviceId, const Json::Value& message) {
        registry.pingService(serviceId);
    });
}

ServiceBroker::~ServiceBroker() {
    stop();
}

bool ServiceBroker::start() {
    if (running.load()) {
        std::cerr << "ServiceBroker is already running" << std::endl;
        return false;
    }
    
    running.store(true);
    
    std::cout << "Starting ServiceBroker..." << std::endl;
    std::cout << "TCP Port: " << tcpPort << std::endl;
    std::cout << "UNIX Socket: " << unixSocketPath << std::endl;
    
    // Start network listeners
    tcpListener = std::thread(&ServiceBroker::startTcpListener, this);
    unixListener = std::thread(&ServiceBroker::startUnixListener, this);
    healthCheckThread = std::thread(&ServiceBroker::startHealthChecker, this);
    
    std::cout << "ServiceBroker started successfully" << std::endl;
    return true;
}

void ServiceBroker::stop() {
    if (!running.load()) {
        return;
    }
    
    std::cout << "Stopping ServiceBroker..." << std::endl;
    running.store(false);
    
    // Close all active connections
    std::lock_guard lock(connectionsMutex);
    for (const auto &fd: activeConnections | std::views::keys) {
        close(fd);
    }
    activeConnections.clear();
    
    // Wait for threads to finish
    if (tcpListener.joinable()) tcpListener.join();
    if (unixListener.joinable()) unixListener.join();
    if (healthCheckThread.joinable()) healthCheckThread.join();
    
    // Clean up UNIX socket file
    unlink(unixSocketPath.c_str());
    
    std::cout << "ServiceBroker stopped" << std::endl;
}

void ServiceBroker::startTcpListener() {
    const int serverFd = createTcpSocket();
    if (serverFd == -1) {
        std::cerr << "Failed to create TCP socket" << std::endl;
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(tcpPort);
    
    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        std::cerr << "TCP bind failed on port " << tcpPort << std::endl;
        close(serverFd);
        return;
    }
    
    if (listen(serverFd, 10) < 0) {
        std::cerr << "TCP listen failed" << std::endl;
        close(serverFd);
        return;
    }
    
    std::cout << "TCP listener started on port " << tcpPort << std::endl;
    
    while (running.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        
        int clientFd = accept(serverFd, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (running.load()) {
                std::cerr << "TCP accept failed" << std::endl;
            }
            continue;
        }
        
        // Get client IP
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        
        const std::string tcpTarget = std::string(clientIP) + ":" + std::to_string(ntohs(clientAddr.sin_port));
        handleNewConnection(clientFd, tcpTarget, "tcp");
    }
    
    close(serverFd);
}

void ServiceBroker::startUnixListener() {
    const int serverFd = createUnixSocket();
    if (serverFd == -1) {
        std::cerr << "Failed to create UNIX socket" << std::endl;
        return;
    }
    
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, unixSocketPath.c_str(), sizeof(address.sun_path) - 1);
    
    // Remove existing socket file
    unlink(unixSocketPath.c_str());
    
    if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        std::cerr << "UNIX bind failed on " << unixSocketPath << std::endl;
        close(serverFd);
        return;
    }
    
    if (listen(serverFd, 10) < 0) {
        std::cerr << "UNIX listen failed" << std::endl;
        close(serverFd);
        return;
    }
    
    std::cout << "UNIX listener started on " << unixSocketPath << std::endl;
    
    while (running.load()) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (running.load()) {
                std::cerr << "UNIX accept failed" << std::endl;
            }
            continue;
        }
        
        handleNewConnection(clientFd, unixSocketPath, "unix");
    }
    
    close(serverFd);
}

void ServiceBroker::startHealthChecker() {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (running.load()) {
            performHealthCheck();
            cleanupExpiredRequests();
        }
    }
}

void ServiceBroker::handleNewConnection(int clientFd, const std::string& address, const std::string& type) {
    std::lock_guard<std::mutex> lock(connectionsMutex);
        
    activeConnections[clientFd] = {.socketFd = clientFd, .address = address, .connectionType = type};
    
    std::cout << "New " << type << " connection from " << address << " (fd: " << clientFd << ")" << std::endl;
    
    // Start listening for messages from this client
    std::thread([this, clientFd]() {
        char buffer[4096];
        while (running.load()) {
            const ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                closeConnection(clientFd);
                break;
            }
            
            buffer[bytesRead] = '\0';
            std::string message(buffer);
            handleClientMessage(clientFd, message);
        }
    }).detach();
}

void ServiceBroker::handleClientMessage(int clientFd, const std::string& message) {
    try {
        Json::Value jsonMessage;
        const Json::CharReaderBuilder reader;
        Json::String errs;

        if (std::istringstream iss(message); !Json::parseFromStream(reader, iss, &jsonMessage, &errs)) {
            std::cerr << "Invalid JSON from client fd=" << clientFd << ": " << errs << std::endl;
            return;
        }

        if (const std::string messageType = jsonMessage["type"].asString(); messageType == "IDENTIFY") {
            handleIdentifyMessage(clientFd, jsonMessage);
        } else if (messageType == "PING") {
            handlePingMessage(clientFd, jsonMessage);
        } else if (messageType == "RESPONSE") {
            handleResponseMessage(clientFd, jsonMessage);
        } else {
            std::cerr << "Unknown message type: " << messageType << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error handling message from fd=" << clientFd << ": " << e.what() << std::endl;
    }
}

bool ServiceBroker::handleIdentifyMessage(const int clientFd, const Json::Value& message) {
    try {
        // Parse service identity from message
        ServiceIdentity identity = ServiceIdentity::fromJson(message["identity"]);
        
        // Validate required fields
        if (identity.serviceId.empty() || identity.serviceName.empty()) {
            std::cerr << "Invalid identification: missing serviceId or serviceName" << std::endl;
            return false;
        }
        
        // Set connection info
        {
            std::lock_guard lock(connectionsMutex);
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
            Json::Value ackMessage;
            ackMessage["type"] = "ACKNOWLEDGED";
            ackMessage["serviceId"] = identity.serviceId;
            ackMessage["status"] = "registered";

            const Json::StreamWriterBuilder builder;
            const std::string ackStr = Json::writeString(builder, ackMessage);
            
            sendMessage(clientFd, ackStr);
            
            std::cout << "Service registered: " << identity.serviceId 
                      << " (" << identity.serviceName << ")" << std::endl;
            return true;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in handleIdentifyMessage: " << e.what() << std::endl;
    }
    
    return false;
}

bool ServiceBroker::handlePingMessage(const int clientFd, const Json::Value& message) {
    std::string serviceId;
    
    // Find serviceId for this connection
    {
        std::lock_guard lock(connectionsMutex);
        if (const auto it = activeConnections.find(clientFd); it != activeConnections.end() && it->second.identified) {
            serviceId = it->second.serviceId;
        }
    }
    
    if (!serviceId.empty()) {
        // Update stats if provided
        if (message.isMember("stats")) {
            const auto& stats = message["stats"];
            if (stats.isMember("currentLoad")) {
                const uint32_t load = stats["currentLoad"].asUInt();
                registry.updateServiceStats(serviceId, load, std::chrono::milliseconds(0));
            }
        }
        
        // Update ping time
        registry.pingService(serviceId);
        
        // Send pong
        Json::Value pongMessage;
        pongMessage["type"] = "PONG";
        pongMessage["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        const Json::StreamWriterBuilder builder;
        const std::string pongStr = Json::writeString(builder, pongMessage);
        
        return sendMessage(clientFd, pongStr);
    }
    
    return false;
}

bool ServiceBroker::handleResponseMessage(const int clientFd, const Json::Value& message) {
    const std::string requestId = message["requestId"].asString();
    
    // TODO: Handle response forwarding to original client
    // For now, just log it
    std::cout << "Received response for request " << requestId 
              << " from fd=" << clientFd << std::endl;
    
    return true;
}

void ServiceBroker::closeConnection(const int clientFd) {
    std::lock_guard lock(connectionsMutex);

    if (const auto it = activeConnections.find(clientFd); it != activeConnections.end()) {
        std::cout << "Closing connection fd=" << clientFd 
                  << " (" << it->second.address << ")" << std::endl;
        
        // Unregister service if it was identified
        if (it->second.identified && !it->second.serviceId.empty()) {
            registry.unregisterService(it->second.serviceId);
        }
        
        close(clientFd);
        activeConnections.erase(it);
    }
}

Json::Value ServiceBroker::getBrokerStatus() const {
    Json::Value status;
    status["running"] = running.load();
    status["tcpPort"] = tcpPort;
    status["unixSocket"] = unixSocketPath;
    status["activeConnections"] = static_cast<int>(getActiveConnectionCount());
    status["registryStatus"] = registry.getRegistryStatus();
    
    return status;
}

Json::Value ServiceBroker::getConnectionStatus() const {
    std::lock_guard lock(connectionsMutex);
    
    Json::Value connections(Json::arrayValue);
    for (const auto& [fd, conn] : activeConnections) {
        Json::Value connInfo;
        connInfo["fd"] = fd;
        connInfo["address"] = conn.address;
        connInfo["type"] = conn.connectionType;
        connInfo["serviceId"] = conn.serviceId;
        connInfo["identified"] = conn.identified;

        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - conn.connectedAt).count();
        connInfo["uptimeSeconds"] = uptime;
        
        connections.append(connInfo);
    }
    
    return connections;
}

size_t ServiceBroker::getActiveConnectionCount() const {
    std::lock_guard lock(connectionsMutex);
    return activeConnections.size();
}

void ServiceBroker::setMessageHandler(const std::string& messageType, MessageHandler handler) {
    messageHandlers[messageType] = std::move(handler);
}

// Helper methods
bool ServiceBroker::sendMessage(const int socketFd, const std::string& message) {
    const ssize_t sent = send(socketFd, message.c_str(), message.length(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(message.length());
}

std::string ServiceBroker::generateRequestId() {
    return "req_" + std::to_string(requestIdCounter.fetch_add(1));
}

void ServiceBroker::cleanupExpiredRequests() {
    // TODO: Implement request timeout cleanup
}

void ServiceBroker::performHealthCheck() {
    // Remove unhealthy services
    registry.removeUnhealthyServices(std::chrono::seconds(60));
}

int ServiceBroker::createTcpSocket() {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd == -1) {
        return -1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    return socketFd;
}

int ServiceBroker::createUnixSocket() {
    return socket(AF_UNIX, SOCK_STREAM, 0);
}

} // namespace servicebroker
