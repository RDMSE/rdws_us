//
// Example Service - ServiceBroker Architecture v2.0
// Demonstrates service creation using ServiceClient library
//

#include "../../service_broker/Services/ServiceClient.h"
#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <chrono>
#include <random>
#include <utility>

using namespace servicebroker;

class ExampleService {
private:
    ServiceIdentity identity;
    std::unique_ptr<ServiceClient> client;
    std::string brokerAddress;
    bool developmentMode = false;
    std::atomic<bool> running{false};
    
public:
    ExampleService(const std::string& serviceId, const std::string& machineName,
                   const bool devMode = false, std::string  broker = "unix:///tmp/service_broker.sock")
        : brokerAddress(std::move(broker)), developmentMode(devMode) {
        
        // Setup service identity
        identity.machineName = machineName;
        identity.serviceName = "example_service";
        identity.serviceId = serviceId;
        identity.version = "v2.0.0";
        identity.environment = devMode ? "dev" : "prod";
        identity.maxConcurrent = 10;
        identity.capabilities = {"ping", "echo", "status", "info", "math"};
    }
    
    bool initialize() {
        std::cout << "[" << identity.serviceId << "] Initializing service..." << std::endl;
        
        client = std::make_unique<ServiceClient>(identity, brokerAddress);
        
        client->setRequestHandler([this](const Json::Value& request) -> Json::Value {
            return this->processRequest(request);
        });
        
        if (developmentMode) {
            std::cout << "[" << identity.serviceId << "] Debug mode enabled" << std::endl;
        }
        
        return true;
    }
    
    void run() {
        running.store(true);
        
        std::cout << "[" << identity.serviceId << "] Starting service..." << std::endl;
        std::cout << "[" << identity.serviceId << "] Machine: " << identity.machineName << std::endl;
        std::cout << "[" << identity.serviceId << "] Version: " << identity.version << std::endl;
        std::cout << "[" << identity.serviceId << "] Capabilities: ";
        
        for (size_t i = 0; i < identity.capabilities.size(); ++i) {
            std::cout << identity.capabilities[i];
            if (i < identity.capabilities.size() - 1) std::cout << ", ";
        }
        std::cout << std::endl;
        std::cout << "[" << identity.serviceId << "] Broker: " << brokerAddress << std::endl;
        
        client->run();
        
        std::cout << "[" << identity.serviceId << "] Service stopped" << std::endl;
    }
    
    void shutdown() {
        std::cout << "[" << identity.serviceId << "] Shutdown requested" << std::endl;
        running.store(false);
        
        if (client) {
            client->stop();
        }
    }
    
private:
    [[nodiscard]] Json::Value processRequest(const Json::Value& request) const {
        const std::string command = request.get("command", "").asString();
        
        std::cout << "[" << identity.serviceId << "] Processing: " << command << std::endl;
        
        Json::Value response;
        response["serviceId"] = identity.serviceId;
        response["timestamp"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        
        if (command == "ping") {
            response["result"] = "pong";
            response["status"] = "success";
            
        } else if (command == "status") {
            response["result"] = "running";
            response["status"] = "success";
            response["uptime"] = static_cast<Json::Int64>(identity.getUptime().count());
            response["load"] = static_cast<Json::Int64>(identity.currentLoad);
            
        } else if (command == "info") {
            response["result"] = Json::Value();
            response["result"]["service"] = identity.serviceName;
            response["result"]["id"] = identity.serviceId;
            response["result"]["machine"] = identity.machineName;
            response["result"]["version"] = identity.version;
            response["result"]["environment"] = identity.environment;
            response["result"]["capabilities"] = Json::Value(Json::arrayValue);
            for (const auto& cap : identity.capabilities) {
                response["result"]["capabilities"].append(cap);
            }
            response["status"] = "success";
            
        } else if (command == "echo") {
            std::string message = request.get("message", "").asString();
            response["result"] = "echo: " + message;
            response["status"] = "success";
            
        } else {
            response["error"] = "Unknown command: " + command;
            response["status"] = "error";
            response["availableCommands"] = Json::Value(Json::arrayValue);
            response["availableCommands"].append("ping");
            response["availableCommands"].append("status");
            response["availableCommands"].append("info");
            response["availableCommands"].append("echo");
        }
        
        // Simulate processing time
        std::this_thread::sleep_for(std::chrono::milliseconds(10 + (rand() % 50)));
        
        std::cout << "[" << identity.serviceId << "] Response: " << response["status"].asString() << std::endl;
        
        return response;
    }
};

// Global service instance for signal handling
ExampleService* globalService = nullptr;

void signalHandler(int signal) {
    if (globalService && (signal == SIGTERM || signal == SIGINT)) {
        globalService->shutdown();
    }
}

int main(const int argc, char* argv[]) {
    bool developmentMode = false;
    std::string brokerAddress = "unix:///tmp/service_broker.sock";
    std::string serviceId = "example_001";
    std::string machineName = "localhost";
    
    // Parse arguments
    if (argc >= 2 && std::string(argv[1]) == "--dev") {
        developmentMode = true;
        serviceId = "example_dev";
        machineName = "dev-machine";
        
        std::cout << "=== DEVELOPMENT MODE ===" << std::endl;
        std::cout << "Connect to broker for debugging" << std::endl;
        std::cout << "========================" << std::endl;
        
    } else if (argc >= 4) {
        serviceId = argv[1];
        machineName = argv[2];
        brokerAddress = argv[3];
        
    } else if (argc != 1) {
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  Development: " << argv[0] << " --dev" << std::endl;
        std::cerr << "  Production:  " << argv[0] << " <serviceId> <machineName> <brokerAddress>" << std::endl;
        std::cerr << "  Default:     " << argv[0] << std::endl;
        return 1;
    }
    
    std::cout << "[" << serviceId << "] Example Service v2.0 (ServiceBroker Architecture)" << std::endl;
    
    ExampleService service(serviceId, machineName, developmentMode, brokerAddress);
    globalService = &service;
    
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
    
    if (!service.initialize()) {
        std::cerr << "[" << serviceId << "] Failed to initialize service" << std::endl;
        return 1;
    }
    
    service.run();
    
    std::cout << "[" << serviceId << "] Service exited gracefully" << std::endl;
    return 0;
}
