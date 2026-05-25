#include "ServiceGateway.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <unistd.h>
#include <cmath>
#include <iostream>
#include <ranges>
#include <utility>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "../../shared/utils/logger.h"

namespace servicegateway
{

    ServiceGateway::ServiceGateway(const int port, std::string unixSocket)
        : tcpPort(port), unixSocketPath(std::move(unixSocket))
    {

        // Set up default message handlers
        setMessageHandler("IDENTIFY", [this](const std::string &serviceId, const rapidjson::Document &message)
                          {
                              // Already handled in handleIdentifyMessage
                          });

        setMessageHandler("PING", [this](const std::string &serviceId, const rapidjson::Document &message)
                          { registry.pingService(serviceId); });
    }

    ServiceGateway::~ServiceGateway()
    {
        stop();
    }

    bool ServiceGateway::start()
    {
        if (running.load())
        {
            std::cerr << "ServiceGateway is already running" << '\n';
            return false;
        }

        running.store(true);

        std::cout << "Starting ServiceGateway..." << '\n';
        std::cout << "TCP Port: " << tcpPort << '\n';
        std::cout << "UNIX Socket: " << unixSocketPath << '\n';

        // Start network listeners
        tcpListener = std::thread(&ServiceGateway::startTcpListener, this);
        unixListener = std::thread(&ServiceGateway::startUnixListener, this);
        healthCheckThread = std::thread(&ServiceGateway::startHealthChecker, this);

        std::cout << "ServiceGateway started successfully" << '\n';
        return true;
    }

    void ServiceGateway::stop()
    {
        if (!running.load())
        {
            return;
        }

        std::cout << "Stopping ServiceGateway..." << '\n';
        running.store(false);

        // Unblock select()/accept() in listener threads.
        if (const int fd = tcpServerFd.exchange(-1); fd != -1) close(fd);
        if (const int fd = unixServerFd.exchange(-1); fd != -1) close(fd);

        // Close all active connections (scoped so the mutex is released before joining).
        {
            std::scoped_lock lock(connectionsMutex);
            for (const auto &fd : activeConnections | std::views::keys)
                close(fd);
            activeConnections.clear();
        }

        // Wait for threads to finish (mutex NOT held here).
        if (tcpListener.joinable())      tcpListener.join();
        if (unixListener.joinable())     unixListener.join();
        if (healthCheckThread.joinable()) healthCheckThread.join();

        // Clean up UNIX socket file
        unlink(unixSocketPath.c_str());

        std::cout << "ServiceGateway stopped" << '\n';
    }

    void ServiceGateway::startTcpListener()
    {
        const int serverFd = createTcpSocket();
        if (serverFd == -1)
        {
            std::cerr << "Failed to create TCP socket" << '\n';
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(tcpPort);

        if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        {
            std::cerr << "TCP bind failed on port " << tcpPort << '\n';
            close(serverFd);
            return;
        }

        if (listen(serverFd, 10) < 0)
        {
            std::cerr << "TCP listen failed" << '\n';
            close(serverFd);
            return;
        }

        std::cout << "TCP listener started on port " << tcpPort << '\n';
        tcpServerFd = serverFd;

        while (running.load())
        {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(serverFd, &readfds);
            struct timeval tv{1, 0}; // 1-second timeout so stop() unblocks quickly
            if (select(serverFd + 1, &readfds, nullptr, nullptr, &tv) <= 0)
                continue;

            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            const int clientFd = accept(serverFd, reinterpret_cast<struct sockaddr *>(&clientAddr), &clientLen);
            if (clientFd < 0)
            {
                if (running.load())
                    std::cerr << "TCP accept failed" << '\n';
                continue;
            }

            // Get client IP
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

            const std::string tcpTarget = std::string(clientIP) + ":" + std::to_string(ntohs(clientAddr.sin_port));
            handleNewConnection(clientFd, tcpTarget, "tcp");
        }

        tcpServerFd = -1;
        close(serverFd);
    }

    void ServiceGateway::startUnixListener()
    {
        const int serverFd = createUnixSocket();
        if (serverFd == -1)
        {
            std::cerr << "Failed to create UNIX socket" << '\n';
            return;
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, unixSocketPath.c_str(), sizeof(address.sun_path) - 1);

        // Remove existing socket file
        unlink(unixSocketPath.c_str());

        if (bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
        {
            std::cerr << "UNIX bind failed on " << unixSocketPath << '\n';
            close(serverFd);
            return;
        }

        if (listen(serverFd, 10) < 0)
        {
            std::cerr << "UNIX listen failed" << '\n';
            close(serverFd);
            return;
        }

        std::cout << "UNIX listener started on " << unixSocketPath << '\n';
        unixServerFd = serverFd;

        while (running.load())
        {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(serverFd, &readfds);
            struct timeval tv{1, 0};
            if (select(serverFd + 1, &readfds, nullptr, nullptr, &tv) <= 0)
                continue;

            const int clientFd = accept(serverFd, nullptr, nullptr);
            if (clientFd < 0)
            {
                if (running.load())
                    std::cerr << "UNIX accept failed" << '\n';
                continue;
            }

            handleNewConnection(clientFd, unixSocketPath, "unix");
        }

        unixServerFd = -1;
        close(serverFd);
    }

    void ServiceGateway::startHealthChecker()
    {
        while (running.load())
        {
            // Sleep in short increments so stop() doesn't wait 30 seconds.
            for (int i = 0; i < 30 && running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (running.load())
            {
                performHealthCheck();
                cleanupExpiredRequests();
            }
        }
    }

    void ServiceGateway::handleNewConnection(const int clientFd, const std::string &address, const std::string &type)
    {
        std::scoped_lock lock(connectionsMutex);

        activeConnections[clientFd] = {.socketFd = clientFd, .address = address, .connectionType = type};

        std::cout << "New " << type << " connection from " << address << " (fd: " << clientFd << ")" << '\n';

        // Start listening for messages from this client
        std::thread([this, clientFd]()
                    {
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
        } })
            .detach();
    }

    void ServiceGateway::handleClientMessage(const int clientFd, const std::string &message)
    {
        try
        {
            rapidjson::Document jsonMessage;
            jsonMessage.Parse(message.c_str());

            if (jsonMessage.HasParseError() || !jsonMessage.IsObject())
            {
                std::cerr << "Invalid JSON from client fd=" << clientFd << '\n';
                return;
            }

            if (!jsonMessage.HasMember("type") || !jsonMessage["type"].IsString())
            {
                std::cerr << "Missing message type from client fd=" << clientFd << '\n';
                return;
            }

            if (const std::string messageType = jsonMessage["type"].GetString(); messageType == "IDENTIFY")
            {
                handleIdentifyMessage(clientFd, jsonMessage);
            }
            else if (messageType == "PING")
            {
                handlePingMessage(clientFd, jsonMessage);
            }
            else if (messageType == "RESPONSE")
            {
                handleResponseMessage(clientFd, jsonMessage);
            }
            else
            {
                std::cerr << "Unknown message type: " << messageType << '\n';
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error handling message from fd=" << clientFd << ": " << e.what() << '\n';
        }
    }

    bool ServiceGateway::handleIdentifyMessage(const int clientFd, const rapidjson::Document &message)
    {
        try
        {
            if (!message.HasMember("identity") || !message["identity"].IsObject())
            {
                std::cerr << "Invalid identification: missing identity object" << '\n';
                return false;
            }

            // Parse service identity from message
            ServiceIdentity identity = ServiceIdentity::fromJson(message["identity"]);

            // Validate required fields
            if (identity.serviceId.empty() || identity.serviceName.empty())
            {
                std::cerr << "Invalid identification: missing serviceId or serviceName" << '\n';
                return false;
            }

            // Set connection info
            {
                std::scoped_lock lock(connectionsMutex);
                auto it = activeConnections.find(clientFd);
                if (it != activeConnections.end())
                {
                    identity.connectionType = it->second.connectionType;
                    identity.clientAddress = it->second.address;
                    it->second.serviceId = identity.serviceId;
                    it->second.identified = true;
                }
            }

            // Register in service registry
            if (registry.registerService(identity))
            {
                // Send acknowledgment
                rapidjson::Document ackMessage;
                ackMessage.SetObject();
                auto &allocator = ackMessage.GetAllocator();
                ackMessage.AddMember("type", "ACKNOWLEDGED", allocator);
                ackMessage.AddMember("serviceId", rapidjson::Value(identity.serviceId.c_str(), allocator), allocator);
                ackMessage.AddMember("status", "registered", allocator);

                rapidjson::StringBuffer buffer;
                rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                ackMessage.Accept(writer);

                sendMessage(clientFd, buffer.GetString());

                std::cout << "Service registered: " << identity.serviceId
                          << " (" << identity.serviceName << ")" << '\n';
                rdws::logger::serviceConnected(identity.serviceId,
                                               identity.serviceName,
                                               identity.clientAddress);
                return true;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error in handleIdentifyMessage: " << e.what() << '\n';
        }

        return false;
    }

    bool ServiceGateway::handlePingMessage(const int clientFd, const rapidjson::Document &message)
    {
        std::string serviceId;

        // Find serviceId for this connection
        {
            std::scoped_lock lock(connectionsMutex);
            if (const auto it = activeConnections.find(clientFd); it != activeConnections.end() && it->second.identified)
            {
                serviceId = it->second.serviceId;
            }
        }

        if (!serviceId.empty())
        {
            // Update stats if provided
            if (message.HasMember("stats") && message["stats"].IsObject())
            {
                const auto &stats = message["stats"];
                if (stats.HasMember("currentLoad") && stats["currentLoad"].IsUint())
                {
                    const uint32_t load = stats["currentLoad"].GetUint();
                    registry.updateCurrentLoad(serviceId, load);
                }
            }

            // Update ping time
            registry.pingService(serviceId);

            // Send pong
            rapidjson::Document pongMessage;
            pongMessage.SetObject();
            auto &allocator = pongMessage.GetAllocator();
            pongMessage.AddMember("type", "PONG", allocator);
            pongMessage.AddMember("timestamp", static_cast<int64_t>(
                                      std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count()),
                                  allocator);

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            pongMessage.Accept(writer);

            return sendMessage(clientFd, buffer.GetString());
        }

        return false;
    }

    bool ServiceGateway::handleResponseMessage(const int clientFd, const rapidjson::Document &message)
    {
        if (!message.HasMember("requestId") || !message["requestId"].IsString())
        {
            std::cerr << "Received RESPONSE without valid requestId from fd=" << clientFd << '\n';
            return false;
        }

        const std::string requestId = message["requestId"].GetString();
        const auto now = std::chrono::steady_clock::now();

        std::scoped_lock lock(requestsMutex);
        const auto it = pendingRequests.find(requestId);
        if (it == pendingRequests.end())
        {
            std::cerr << "Received response for unknown requestId=" << requestId
                      << " from fd=" << clientFd << '\n';
            return false;
        }

        PendingRequest &pending = it->second;
        pending.updatedAt = now;

        if (message.HasMember("data"))
        {
            pending.responsePayload = serializeJsonValue(message["data"]);
        }

        bool isError = false;
        std::string errorMessage;

        if (message.HasMember("error") && message["error"].IsString())
        {
            isError = true;
            errorMessage = message["error"].GetString();
        }

        if (message.HasMember("data") && message["data"].IsObject())
        {
            const auto &data = message["data"];
            if (data.HasMember("status") && data["status"].IsString() && std::string(data["status"].GetString()) == "error")
            {
                isError = true;
            }
            if (data.HasMember("error") && data["error"].IsString())
            {
                isError = true;
                errorMessage = data["error"].GetString();
            }
        }

        const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - pending.createdAt);
        registry.recordResponseTime(pending.targetServiceId, latencyMs);

        if (isError)
        {
            pending.state = RequestState::FAILED;
            pending.statusCode = 500;
            pending.errorMessage = errorMessage.empty() ? "Service returned an error response" : errorMessage;
            registry.recordServiceError(pending.targetServiceId);
        }
        else
        {
            pending.state = RequestState::COMPLETED;
            pending.statusCode = 200;
            pending.errorMessage.clear();
        }

        std::cout << "Received response for request " << requestId
                  << " from fd=" << clientFd
                  << " state=" << requestStateToString(pending.state) << '\n';

        // Notify any HTTP thread waiting on this requestId
        auto waiterIt = responseWaiters_.find(requestId);
        if (waiterIt != responseWaiters_.end()) {
            waiterIt->second->notify_one();
        }

        rdws::logger::responseCorrelated(requestId,
                                         pending.targetServiceId,
                                         requestStateToString(pending.state));

        return true;
    }

    PendingRequest ServiceGateway::waitForResponse(const std::string& requestId,
                                                   const std::chrono::milliseconds timeout)
    {
        auto cv = std::make_shared<std::condition_variable>();

        std::unique_lock lock(requestsMutex);
        responseWaiters_[requestId] = cv;

        const bool completed = cv->wait_for(lock, timeout, [&]() {
            const auto it = pendingRequests.find(requestId);
            if (it == pendingRequests.end())
            {
                return true;
            }
            return it->second.state == RequestState::COMPLETED ||
                   it->second.state == RequestState::FAILED ||
                   it->second.state == RequestState::TIMED_OUT;
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
                                           const LoadBalancingStrategy strategy)
    {
        // Apply routing rules: may remap the incoming capability to another.
        const std::string resolvedCapability = router_.resolve(capability, requestData);
        const std::string targetServiceId = registry.selectBestService(resolvedCapability, strategy);
        if (targetServiceId.empty())
        {
            std::cerr << "No available service for capability: " << resolvedCapability << '\n';
            return "";
        }

        std::string requestId = generateRequestId();

        PendingRequest pending;
        pending.requestId = requestId;
        pending.targetServiceId = targetServiceId;
        pending.capability = resolvedCapability;
        pending.requestPayload = serializeJsonValue(requestData);
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
        auto &allocator = requestMessage.GetAllocator();
        requestMessage.AddMember("type", "REQUEST", allocator);
        requestMessage.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);
        requestMessage.AddMember("capability", rapidjson::Value(resolvedCapability.c_str(), allocator), allocator);

        rapidjson::Value data;
        data.CopyFrom(requestData, allocator);
        requestMessage.AddMember("data", data, allocator);

        if (!sendDirectRequest(targetServiceId, requestMessage))
        {
            std::scoped_lock lock(requestsMutex);
            pendingRequests.erase(requestId);
            return "";
        }

        {
            std::scoped_lock lock(requestsMutex);
            auto it = pendingRequests.find(requestId);
            if (it != pendingRequests.end() && it->second.state == RequestState::QUEUED)
            {
                it->second.state = RequestState::IN_FLIGHT;
                it->second.updatedAt = std::chrono::steady_clock::now();
            }
        }

        return requestId;
    }

    bool ServiceGateway::sendDirectRequest(const std::string& serviceId, const rapidjson::Document& requestData)
    {
        int targetFd = -1;
        {
            std::scoped_lock lock(connectionsMutex);
            for (const auto &[fd, conn] : activeConnections)
            {
                if (conn.identified && conn.serviceId == serviceId)
                {
                    targetFd = fd;
                    break;
                }
            }
        }

        if (targetFd == -1)
        {
            std::cerr << "Service not connected: " << serviceId << '\n';
            return false;
        }

        rapidjson::Document outbound;
        if (requestData.IsObject() && requestData.HasMember("type") && requestData["type"].IsString())
        {
            outbound.CopyFrom(requestData, outbound.GetAllocator());
        }
        else
        {
            outbound.SetObject();
            auto &allocator = outbound.GetAllocator();
            outbound.AddMember("type", "REQUEST", allocator);
            const std::string requestId = generateRequestId();
            outbound.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);
            rapidjson::Value data;
            data.CopyFrom(requestData, allocator);
            outbound.AddMember("data", data, allocator);
        }

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        outbound.Accept(writer);
        return sendMessage(targetFd, buffer.GetString());
    }

    void ServiceGateway::closeConnection(const int clientFd)
    {
        std::string disconnectedServiceId;

        {
            std::scoped_lock lock(connectionsMutex);

            if (const auto it = activeConnections.find(clientFd); it != activeConnections.end())
            {
                std::cout << "Closing connection fd=" << clientFd
                          << " (" << it->second.address << ")" << '\n';

                if (it->second.identified && !it->second.serviceId.empty())
                {
                    disconnectedServiceId = it->second.serviceId;
                    rdws::logger::serviceDisconnected(it->second.serviceId, "connection closed");
                    registry.unregisterService(it->second.serviceId);
                }

                close(clientFd);
                activeConnections.erase(it);
            }
        }

        // Fail any in-flight requests that were routed to the disconnected service
        if (!disconnectedServiceId.empty())
        {
            std::scoped_lock lock(requestsMutex);
            for (auto &[reqId, pending] : pendingRequests)
            {
                if (pending.targetServiceId == disconnectedServiceId &&
                    (pending.state == RequestState::QUEUED || pending.state == RequestState::IN_FLIGHT))
                {
                    pending.state     = RequestState::FAILED;
                    pending.statusCode = 503;
                    pending.errorMessage = "Service disconnected";

                    if (const auto waiterIt = responseWaiters_.find(reqId);
                        waiterIt != responseWaiters_.end())
                    {
                        waiterIt->second->notify_one();
                    }
                }
            }
        }
    }

    rapidjson::Document ServiceGateway::getGatewayStatus() const
    {
        rapidjson::Document status;
        status.SetObject();
        auto &allocator = status.GetAllocator();

        status.AddMember("running", running.load(), allocator);
        status.AddMember("tcpPort", tcpPort, allocator);
        status.AddMember("unixSocket", rapidjson::Value(unixSocketPath.c_str(), allocator), allocator);
        status.AddMember("activeConnections", static_cast<int>(getActiveConnectionCount()), allocator);
        status.AddMember("trackedRequests", static_cast<int>(getTrackedRequestCount()), allocator);

        rapidjson::Document registryStatus = registry.getRegistryStatus();
        rapidjson::Value registryValue;
        registryValue.CopyFrom(registryStatus, allocator);
        status.AddMember("registryStatus", registryValue, allocator);

        return status;
    }

    rapidjson::Document ServiceGateway::getConnectionStatus() const
    {
        std::scoped_lock lock(connectionsMutex);

        rapidjson::Document connections;
        connections.SetArray();
        auto &allocator = connections.GetAllocator();
        for (const auto &[fd, conn] : activeConnections)
        {
            rapidjson::Value connInfo(rapidjson::kObjectType);
            connInfo.AddMember("fd", fd, allocator);
            connInfo.AddMember("address", rapidjson::Value(conn.address.c_str(), allocator), allocator);
            connInfo.AddMember("type", rapidjson::Value(conn.connectionType.c_str(), allocator), allocator);
            connInfo.AddMember("serviceId", rapidjson::Value(conn.serviceId.c_str(), allocator), allocator);
            connInfo.AddMember("identified", conn.identified, allocator);

            const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - conn.connectedAt)
                                    .count();
            connInfo.AddMember("uptimeSeconds", static_cast<int64_t>(uptime), allocator);

            connections.PushBack(connInfo, allocator);
        }

        return connections;
    }

    size_t ServiceGateway::getActiveConnectionCount() const
    {
        std::scoped_lock lock(connectionsMutex);
        return activeConnections.size();
    }

    size_t ServiceGateway::getTrackedRequestCount() const
    {
        std::scoped_lock lock(requestsMutex);
        return pendingRequests.size();
    }

    void ServiceGateway::recordMetric(const std::string &capability,
                                      const double latencyMs,
                                      const bool success,
                                      const bool timedOut)
    {
        metrics_.record(capability, latencyMs, success, timedOut);
    }

    rapidjson::Document ServiceGateway::getMetrics() const
    {
        return metrics_.toJson();
    }

    rapidjson::Document ServiceGateway::getHealth() const
    {
        rapidjson::Document doc;
        doc.SetObject();
        auto &alloc = doc.GetAllocator();

        const auto uptimeSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        doc.AddMember("status", running.load() ? "healthy" : "stopped", alloc);
        doc.AddMember("uptimeEpochSec", static_cast<int64_t>(uptimeSec), alloc);

        // Gateway config
        rapidjson::Value gw(rapidjson::kObjectType);
        gw.AddMember("tcpPort",     tcpPort, alloc);
        gw.AddMember("unixSocket",  rapidjson::Value(unixSocketPath.c_str(), alloc), alloc);
        gw.AddMember("activeConnections", static_cast<int>(getActiveConnectionCount()), alloc);
        gw.AddMember("pendingRequests",   static_cast<int>(getTrackedRequestCount()),   alloc);
        doc.AddMember("gateway", gw, alloc);

        // Services
        rapidjson::Document registryDoc = registry.getRegistryStatus();
        rapidjson::Value services(rapidjson::kArrayType);

        if (registryDoc.HasMember("services") && registryDoc["services"].IsArray()) {
            // Merge registry info with per-capability metrics
            rapidjson::Document metricsDoc = metrics_.toJson();

            // Build a quick lookup: capability -> avgLatencyMs, errorRate
            std::map<std::string, rapidjson::Value *> capMetrics;
            if (metricsDoc.HasMember("capabilities") && metricsDoc["capabilities"].IsArray()) {
                for (auto &entry : metricsDoc["capabilities"].GetArray()) {
                    if (entry.HasMember("capability") && entry["capability"].IsString()) {
                        capMetrics[entry["capability"].GetString()] = &entry;
                    }
                }
            }

            for (const auto &svc : registryDoc["services"].GetArray()) {
                rapidjson::Value entry(rapidjson::kObjectType);

                // Copy core identity fields
                for (const auto &m : {"serviceId", "serviceName", "machineName", "version"}) {
                    if (svc.HasMember(m)) {
                        rapidjson::Value key(m, alloc);
                        rapidjson::Value val;
                        val.CopyFrom(svc[m], alloc);
                        entry.AddMember(key, val, alloc);
                    }
                }

                // Derive per-service aggregate metrics from its capabilities
                double totalAvg = 0.0;
                double totalErrorRate = 0.0;
                int    capCount = 0;

                if (svc.HasMember("capabilities") && svc["capabilities"].IsArray()) {
                    rapidjson::Value caps;
                    caps.CopyFrom(svc["capabilities"], alloc);
                    entry.AddMember("capabilities", caps, alloc);

                    for (const auto &capVal : svc["capabilities"].GetArray()) {
                        const std::string cap = capVal.GetString();
                        if (capMetrics.contains(cap)) {
                            const auto *cm = capMetrics.at(cap);
                            if (cm->HasMember("avgLatencyMs")) totalAvg       += (*cm)["avgLatencyMs"].GetDouble();
                            if (cm->HasMember("errorRate"))    totalErrorRate  += (*cm)["errorRate"].GetDouble();
                            capCount++;
                        }
                    }
                }

                if (capCount > 0) {
                    entry.AddMember("avgLatencyMs", std::round(totalAvg / capCount * 100.0) / 100.0, alloc);
                    entry.AddMember("errorRate",    std::round(totalErrorRate / capCount * 10000.0) / 10000.0, alloc);
                }

                services.PushBack(entry, alloc);
            }
        }
        doc.AddMember("services", services, alloc);

        return doc;
    }

    rapidjson::Document ServiceGateway::getRequestStatus(const std::string &requestId) const
    {
        rapidjson::Document response;
        response.SetObject();
        auto &allocator = response.GetAllocator();

        response.AddMember("requestId", rapidjson::Value(requestId.c_str(), allocator), allocator);

        std::scoped_lock lock(requestsMutex);
        const auto it = pendingRequests.find(requestId);
        if (it == pendingRequests.end())
        {
            response.AddMember("found", false, allocator);
            response.AddMember("status", "not_found", allocator);
            response.AddMember("message", "Request ID not found", allocator);
            return response;
        }

        const PendingRequest &pending = it->second;
        response.AddMember("found", true, allocator);
        response.AddMember("status", rapidjson::Value(requestStateToString(pending.state).c_str(), allocator), allocator);
        response.AddMember("statusCode", pending.statusCode, allocator);
        response.AddMember("targetServiceId", rapidjson::Value(pending.targetServiceId.c_str(), allocator), allocator);
        response.AddMember("capability", rapidjson::Value(pending.capability.c_str(), allocator), allocator);
        response.AddMember("requestPayload", rapidjson::Value(pending.requestPayload.c_str(), allocator), allocator);

        if (!pending.responsePayload.empty())
        {
            response.AddMember("responsePayload", rapidjson::Value(pending.responsePayload.c_str(), allocator), allocator);
        }

        if (!pending.errorMessage.empty())
        {
            response.AddMember("errorMessage", rapidjson::Value(pending.errorMessage.c_str(), allocator), allocator);
        }

        const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pending.createdAt).count();
        response.AddMember("ageMs", static_cast<int64_t>(ageMs), allocator);
        response.AddMember("timeoutMs", static_cast<int64_t>(pending.timeout.count()), allocator);

        return response;
    }

    void ServiceGateway::setMessageHandler(const std::string &messageType, MessageHandler handler)
    {
        messageHandlers[messageType] = std::move(handler);
    }

    // Helper methods
    bool ServiceGateway::sendMessage(const int socketFd, const std::string &message)
    {
        const ssize_t sent = send(socketFd, message.c_str(), message.length(), MSG_NOSIGNAL);
        return std::cmp_equal(sent ,message.length());
    }

    std::string ServiceGateway::requestStateToString(const RequestState state)
    {
        switch (state)
        {
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

    std::string ServiceGateway::serializeJsonValue(const rapidjson::Value &value)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

    std::string ServiceGateway::generateRequestId()
    {
        return "req_" + std::to_string(requestIdCounter.fetch_add(1));
    }

    void ServiceGateway::cleanupExpiredRequests()
    {
        constexpr auto retention = std::chrono::minutes(5);
        const auto now = std::chrono::steady_clock::now();

        std::scoped_lock lock(requestsMutex);
        for (auto it = pendingRequests.begin(); it != pendingRequests.end();)
        {
            PendingRequest &pending = it->second;
            const auto age = now - pending.createdAt;

            if ((pending.state == RequestState::QUEUED || pending.state == RequestState::IN_FLIGHT) &&
                age > pending.timeout)
            {
                pending.state = RequestState::TIMED_OUT;
                pending.statusCode = 504;
                pending.errorMessage = "Request timed out waiting for service response";
                pending.updatedAt = now;
            }

            const bool isTerminal = pending.state == RequestState::COMPLETED ||
                                    pending.state == RequestState::FAILED ||
                                    pending.state == RequestState::TIMED_OUT;

            if (isTerminal && (now - pending.updatedAt) > retention)
            {
                it = pendingRequests.erase(it);
                continue;
            }

            ++it;
        }
    }

    void ServiceGateway::performHealthCheck()
    {
        // Remove unhealthy services
        registry.removeUnhealthyServices(std::chrono::seconds(60));
    }

    int ServiceGateway::createTcpSocket()
    {
        const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
        if (socketFd == -1)
        {
            return -1;
        }

        // Set socket options
        constexpr int opt = 1;
        setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        return socketFd;
    }

    int ServiceGateway::createUnixSocket()
    {
        return socket(AF_UNIX, SOCK_STREAM, 0);
    }

} // namespace servicegateway
