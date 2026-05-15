#include "ServiceClient.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace servicegateway {

ServiceClient::ServiceClient(ServiceIdentity  serviceIdentity, const std::string& address)
    : identity(std::move(serviceIdentity)), gatewayAddress(address) {
}

ServiceClient::~ServiceClient() {
    stop();
}

bool ServiceClient::connect() {
    if (connected.load()) {
        return true;
    }
    
    socketFd = createConnection();
    if (socketFd == -1) {
        std::cerr << "Failed to create connection to " << gatewayAddress << std::endl;
        return false;
    }
    
    connected.store(true);
    std::cout << "Connected to broker at " << gatewayAddress << std::endl;
    
    return true;
}

void ServiceClient::disconnect() {
    if (!connected.load()) {
        return;
    }
    
    connected.store(false);
    registered.store(false);
    
    if (socketFd != -1) {
        close(socketFd);
        socketFd = -1;
    }
    
    std::cout << "Disconnected from broker" << std::endl;
}

bool ServiceClient::registerService() const {
    if (!connected.load()) {
        std::cerr << "Not connected to broker" << std::endl;
        return false;
    }
    
    rapidjson::Document identifyMessage;
    identifyMessage.SetObject();
    auto &allocator = identifyMessage.GetAllocator();
    identifyMessage.AddMember("type", "IDENTIFY", allocator);
    identifyMessage.AddMember("identity", identity.toJsonValue(allocator), allocator);
    
    if (sendMessage(identifyMessage)) {
        std::cout << "Sent identification for service: " << identity.serviceId << std::endl;
        return true;
    }
    
    return false;
}

void ServiceClient::setRequestHandler(const RequestHandler &handler) {
    requestHandler = handler;
}

bool ServiceClient::sendPing() {
    rapidjson::Document empty;
    return sendPing(empty);
}

bool ServiceClient::sendPing(const rapidjson::Document& stats) {
    if (!connected.load()) {
        return false;
    }

    rapidjson::Document pingMessage;
    pingMessage.SetObject();
    auto &allocator = pingMessage.GetAllocator();
    pingMessage.AddMember("type", "PING", allocator);
    pingMessage.AddMember("serviceId", rapidjson::Value(identity.serviceId.c_str(), allocator), allocator);
    pingMessage.AddMember("timestamp", static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()), allocator);

    if (!stats.IsNull() && stats.IsObject()) {
        rapidjson::Value statsValue;
        statsValue.CopyFrom(stats, allocator);
        pingMessage.AddMember("stats", statsValue, allocator);
    }

    return sendMessage(pingMessage);
}

bool ServiceClient::sendResponse(const std::string& requestId, const rapidjson::Document& response) const {
    if (!connected.load()) {
        return false;
    }

    rapidjson::Document responseMessage;
    responseMessage.SetObject();
    auto &allocator = responseMessage.GetAllocator();
    responseMessage.AddMember("type", "RESPONSE", allocator);
    responseMessage.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);
    responseMessage.AddMember("serviceId", rapidjson::Value(identity.serviceId.c_str(), allocator), allocator);
    rapidjson::Value data;
    data.CopyFrom(response, allocator);
    responseMessage.AddMember("data", data, allocator);
    
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
    
    std::cout << "Service client running for " << identity.serviceId << std::endl;
    
    // Wait for threads to finish
    if (messageThread.joinable()) messageThread.join();
    if (pingThread.joinable()) pingThread.join();
}

void ServiceClient::stop() {
    disconnect();
    
    if (messageThread.joinable()) messageThread.join();
    if (pingThread.joinable()) pingThread.join();
}

int ServiceClient::createConnection() const {
    if (gatewayAddress.rfind("tcp://", 0) == 0) {
        // TCP connection
        std::string address = gatewayAddress.substr(6); // Remove "tcp://"
        const size_t colonPos = address.find(':');
        if (colonPos == std::string::npos) {
            std::cerr << "Invalid TCP address format" << std::endl;
            return -1;
        }

        const std::string host = address.substr(0, colonPos);
        const int port = std::stoi(address.substr(colonPos + 1));

        const int sockFd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockFd == -1) {
            std::cerr << "Failed to create TCP socket" << std::endl;
            return -1;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        
        if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
            std::cerr << "Invalid TCP address: " << host << std::endl;
            close(sockFd);
            return -1;
        }
        
        if (::connect(sockFd, reinterpret_cast<struct sockaddr *>(&serverAddr), sizeof(serverAddr)) < 0) {
            std::cerr << "Failed to connect to TCP " << host << ":" << port << std::endl;
            close(sockFd);
            return -1;
        }
        
        return sockFd;
        
    }
    if (gatewayAddress.rfind("unix://", 0) == 0) {
        // UNIX socket connection
        const std::string socketPath = gatewayAddress.substr(7); // Remove "unix://"

        const int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sockFd == -1) {
            std::cerr << "Failed to create UNIX socket" << std::endl;
            return -1;
        }

        sockaddr_un serverAddr{};
        serverAddr.sun_family = AF_UNIX;
        strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
        
        if (::connect(sockFd, reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr)) < 0) {
            std::cerr << "Failed to connect to UNIX socket " << socketPath << std::endl;
            close(sockFd);
            return -1;
        }
        
        return sockFd;
    }

    std::cerr << "Unknown address format: " << gatewayAddress << std::endl;
    return -1;
}

bool ServiceClient::sendMessage(const rapidjson::Document& message) const {
    if (socketFd == -1) {
        return false;
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    message.Accept(writer);
    const std::string messageStr = buffer.GetString();

    const ssize_t sent = send(socketFd, messageStr.c_str(), messageStr.length(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(messageStr.length());
}

void ServiceClient::messageLoop() {
    char buffer[4096];
    
    while (connected.load()) {
        const ssize_t bytesRead = recv(socketFd, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) {
            if (connected.load()) {
                std::cerr << "Connection lost to broker" << std::endl;
            }
            break;
        }
        
        buffer[bytesRead] = '\0';
        std::string message(buffer);
        handleMessage(message);
    }
}

void ServiceClient::pingLoop() {
    while (connected.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        if (connected.load() && registered.load()) {
            // Send ping with current stats
            rapidjson::Document stats;
            stats.SetObject();
            auto &allocator = stats.GetAllocator();
            stats.AddMember("currentLoad", identity.currentLoad, allocator);
            stats.AddMember("totalRequests", identity.totalRequests, allocator);
            stats.AddMember("errorCount", identity.errorCount, allocator);
            
            sendPing(stats);
        }
    }
}

void ServiceClient::handleMessage(const std::string& message) {
    try {
        rapidjson::Document jsonMessage;
        jsonMessage.Parse(message.c_str());

        if (jsonMessage.HasParseError() || !jsonMessage.IsObject()) {
            std::cerr << "Invalid JSON from broker" << std::endl;
            return;
        }

        if (!jsonMessage.HasMember("type") || !jsonMessage["type"].IsString()) {
            std::cout << "Unknown message without type from broker" << std::endl;
            return;
        }

        if (const std::string messageType = jsonMessage["type"].GetString(); messageType == "ACKNOWLEDGED") {
            registered.store(true);
            if (jsonMessage.HasMember("serviceId") && jsonMessage["serviceId"].IsString()) {
                std::cout << "Service registered successfully: " << jsonMessage["serviceId"].GetString() << std::endl;
            } else {
                std::cout << "Service registered successfully" << std::endl;
            }
        } else if (messageType == "REQUEST") {
            handleRequest(jsonMessage);
        } else if (messageType == "PONG") {
            handlePong(jsonMessage);
        } else {
            std::cout << "Unknown message type from broker: " << messageType << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error handling broker message: " << e.what() << std::endl;
    }
}

void ServiceClient::handleRequest(const rapidjson::Document& message) {
    if (!requestHandler) {
        std::cerr << "No request handler set for service" << std::endl;
        return;
    }

    if (!message.HasMember("requestId") || !message["requestId"].IsString() ||
        !message.HasMember("data")) {
        std::cerr << "Invalid REQUEST message from broker" << std::endl;
        return;
    }

    const std::string requestId = message["requestId"].GetString();

    rapidjson::Document requestData;
    requestData.CopyFrom(message["data"], requestData.GetAllocator());
    
    std::cout << "Processing request " << requestId << std::endl;
    
    try {
        // Process request
        rapidjson::Document response = requestHandler(requestData);
        
        // Send response back to broker
        (void)sendResponse(requestId, response);
        
        // Update stats
        identity.totalRequests++;
        identity.currentLoad = std::max(0, static_cast<int>(identity.currentLoad) - 1);
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing request " << requestId << ": " << e.what() << std::endl;
        
        // Send error response
        rapidjson::Document errorResponse;
        errorResponse.SetObject();
        errorResponse.AddMember("error", rapidjson::Value(e.what(), errorResponse.GetAllocator()), errorResponse.GetAllocator());
        (void)sendResponse(requestId, errorResponse);
        
        // Update error count
        identity.errorCount++;
    }
}

void ServiceClient::handlePong(const rapidjson::Document& message) {
    // Just log that we received a pong
    std::cout << "Received PONG from broker" << std::endl;
}

} // namespace servicegateway