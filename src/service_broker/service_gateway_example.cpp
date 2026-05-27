#include "../Services/ServiceGateway.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace servicegateway;

int main()
{
    std::cout << "=== ServiceGateway Example ===" << '\n';

    // Create and start gateway
    ServiceGateway gateway(8080, "/tmp/example_service_gateway.sock");

    if (!gateway.start())
    {
        std::cerr << "Failed to start gateway" << '\n';
        return 1;
    }

    std::cout << "\nGateway started! Waiting for service connections..." << '\n';
    std::cout << "Services can connect via:" << '\n';
    std::cout << "  TCP: localhost:8080" << '\n';
    std::cout << "  UNIX: /tmp/example_service_gateway.sock" << '\n';

    // Monitor gateway status
    std::thread monitorThread([&gateway]()
                              {
        while (gateway.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            
            auto status = gateway.getGatewayStatus();
            std::cout << "\n--- Gateway Status ---" << '\n';
            std::cout << "Active Connections: " << status["activeConnections"].GetInt() << '\n';
            std::cout << "Registered Services: " << status["registryStatus"]["totalServices"].GetInt() << '\n';
            std::cout << "Healthy Services: " << status["registryStatus"]["healthyServices"].GetInt() << '\n';
            
            // Show connection details
            auto connections = gateway.getConnectionStatus();
            if (connections.IsArray() && connections.Size() > 0) {
                std::cout << "\nActive Connections:" << '\n';
                for (const auto& conn : connections.GetArray()) {
                    std::cout << "  fd=" << conn["fd"].GetInt()
                              << " type=" << conn["type"].GetString()
                              << " addr=" << conn["address"].GetString();
                    if (conn["identified"].GetBool()) {
                        std::cout << " serviceId=" << conn["serviceId"].GetString();
                    } else {
                        std::cout << " (not identified)";
                    }
                    std::cout << " uptime=" << conn["uptimeSeconds"].GetInt64() << "s" << '\n';
                }
            }
            
            // Show registered services
            const auto& registry = gateway.getRegistry();
            auto serviceIds = registry.getAllServiceIds();
            if (!serviceIds.empty()) {
                std::cout << "\nRegistered Services:" << '\n';
                for (const auto& serviceId : serviceIds) {
                    if (const auto* identity = registry.findServiceById(serviceId)) {
                        std::cout << "  " << serviceId << " (" << identity->serviceName 
                                  << ") on " << identity->machineName 
                                  << " - Load: " << identity->getLoadPercentage() << "%"
                                  << " - Healthy: " << (identity->isHealthy() ? "Yes" : "No") << '\n';
                        
                        // Show capabilities
                        if (!identity->capabilities.empty()) {
                            std::cout << "    Capabilities: ";
                            for (size_t i = 0; i < identity->capabilities.size(); ++i) {
                                std::cout << identity->capabilities[i];
                                if (i < identity->capabilities.size() - 1) std::cout << ", ";
                            }
                            std::cout << '\n';
                        }
                    }
                }
            }
        } });

    std::cout << "\nPress Enter to stop gateway..." << '\n';
    std::cin.get();

    gateway.stop();
    monitorThread.join();

    std::cout << "Gateway stopped." << '\n';
    return 0;
}