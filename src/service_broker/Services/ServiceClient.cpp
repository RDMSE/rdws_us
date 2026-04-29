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

namespace servicebroker {

ServiceClient::ServiceClient(ServiceIdentity  serviceIdentity, const std::string& address)
    : identity(std::move(serviceIdentity)), brokerAddress(address) {
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
        std::cerr << "Failed to create connection to " << brokerAddress << std::endl;
        return false;
    }
    
    connected.store(true);
    std::cout << "Connected to broker at " << brokerAddress << std::endl;
    
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

bool ServiceClient::registerService() {
    if (!connected.load()) {
        std::cerr << "Not connected to broker" << std::endl;
        return false;
    }
    
    Json::Value identifyMessage;
    identifyMessage["type"] = "IDENTIFY";
    identifyMessage["identity"] = identity.toJson();
    
    if (sendMessage(identifyMessage)) {
        std::cout << "Sent identification for service: " << identity.serviceId << std::endl;
        return true;
    }
    
    return false;
}

void ServiceClient::setRequestHandler(const RequestHandler &handler) {
    requestHandler = handler;
}

bool ServiceClient::sendPing(const Json::Value& stats) {
    if (!connected.load()) {
        return false;
    }
    
    Json::Value pingMessage;
    pingMessage["type"] = "PING";
    pingMessage["serviceId"] = identity.serviceId;
    pingMessage["timestamp"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    
    if (!stats.isNull()) {
        pingMessage["stats"] = stats;
    }
    
    return sendMessage(pingMessage);
}

bool ServiceClient::sendResponse(const std::string& requestId, const Json::Value& response) {
    if (!connected.load()) {
        return false;
    }
    
    Json::Value responseMessage;
    responseMessage["type"] = "RESPONSE";
    responseMessage["requestId"] = requestId;
    responseMessage["serviceId"] = identity.serviceId;
    responseMessage["data"] = response;
    
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
    if (brokerAddress.starts_with("tcp://")) {
        // TCP connection
        std::string address = brokerAddress.substr(6); // Remove "tcp://"
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
    if (brokerAddress.starts_with("unix://")) {
        // UNIX socket connection
        const std::string socketPath = brokerAddress.substr(7); // Remove "unix://"

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

    std::cerr << "Unknown address format: " << brokerAddress << std::endl;
    return -1;
}

bool ServiceClient::sendMessage(const Json::Value& message) const {
    if (socketFd == -1) {
        return false;
    }

    const Json::StreamWriterBuilder builder;
    const std::string messageStr = Json::writeString(builder, message);

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
            Json::Value stats;
            stats["currentLoad"] = identity.currentLoad;
            stats["totalRequests"] = identity.totalRequests;
            stats["errorCount"] = identity.errorCount;
            
            sendPing(stats);
        }
    }
}

void ServiceClient::handleMessage(const std::string& message) {
    try {
        Json::Value jsonMessage;
        Json::String errs;
        std::istringstream iss(message);
        
        if (const Json::CharReaderBuilder reader; !Json::parseFromStream(reader, iss, &jsonMessage, &errs)) {
            std::cerr << "Invalid JSON from broker: " << errs << std::endl;
            return;
        }

        if (std::string messageType = jsonMessage["type"].asString(); messageType == "ACKNOWLEDGED") {
            registered.store(true);
            std::cout << "Service registered successfully: " << jsonMessage["serviceId"].asString() << std::endl;
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

void ServiceClient::handleRequest(const Json::Value& message) {
    if (!requestHandler) {
        std::cerr << "No request handler set for service" << std::endl;
        return;
    }

    const std::string requestId = message["requestId"].asString();
    const Json::Value& requestData = message["data"];
    
    std::cout << "Processing request " << requestId << std::endl;
    
    try {
        // Process request
        const Json::Value response = requestHandler(requestData);
        
        // Send response back to broker
        sendResponse(requestId, response);
        
        // Update stats
        identity.totalRequests++;
        identity.currentLoad = std::max(0, static_cast<int>(identity.currentLoad) - 1);
        
    } catch (const std::exception& e) {
        std::cerr << "Error processing request " << requestId << ": " << e.what() << std::endl;
        
        // Send error response
        Json::Value errorResponse;
        errorResponse["error"] = e.what();
        sendResponse(requestId, errorResponse);
        
        // Update error count
        identity.errorCount++;
    }
}

void ServiceClient::handlePong(const Json::Value& message) {
    // Just log that we received a pong
    std::cout << "Received PONG from broker" << std::endl;
}

} // namespace servicebroker