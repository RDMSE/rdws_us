#include "ServiceMonitor.h"
#include <fstream>
#include <thread>
#include <map>
#include <set>

namespace servicebroker {

ServiceMonitor::ServiceMonitor(const ServiceBroker& serviceBroker) : broker(serviceBroker) {}

void ServiceMonitor::displayStatus() const {
    clearScreen();
    printHeader();
    printBrokerStatus();
    printConnectionTable();
    printServiceTable();
    printCapabilityIndex();
}

void ServiceMonitor::displayContinuous(const int refreshIntervalSeconds) const {
    while (broker.isRunning()) {
        displayStatus();
        
        std::cout << "\n" << std::string(80, '-') << std::endl;
        std::cout << "Press Ctrl+C to stop monitoring (refreshing every " 
                  << refreshIntervalSeconds << "s)" << std::endl;
        
        std::this_thread::sleep_for(std::chrono::seconds(refreshIntervalSeconds));
    }
}

void ServiceMonitor::saveStatusToFile(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return;
    }
    
    // Redirect cout to file temporarily
    std::streambuf* origBuf = std::cout.rdbuf();
    std::cout.rdbuf(file.rdbuf());
    
    // Print without clearing screen
    printHeader();
    printBrokerStatus();
    printConnectionTable();
    printServiceTable();
    printCapabilityIndex();
    
    // Restore cout
    std::cout.rdbuf(origBuf);
    
    std::cout << "Status saved to: " << filename << std::endl;
}

void ServiceMonitor::showServicesByCapability(const std::string& capability) const {
    const auto& registry = broker.getRegistry();
    const auto serviceIds = registry.findServicesByCapability(capability);
    
    std::cout << "\n=== Services with capability: " << capability << " ===" << std::endl;
    
    if (serviceIds.empty()) {
        std::cout << "No services found with capability '" << capability << "'" << std::endl;
        return;
    }
    
    std::cout << std::left << std::setw(15) << "Service ID" 
              << std::setw(20) << "Service Name"
              << std::setw(15) << "Machine"
              << std::setw(8) << "Load %"
              << std::setw(10) << "Healthy" << std::endl;
    std::cout << std::string(70, '-') << std::endl;
    
    for (const auto& serviceId : serviceIds) {
        if (const auto* identity = registry.findServiceById(serviceId)) {
            std::cout << std::left << std::setw(15) << serviceId
                      << std::setw(20) << identity->serviceName
                      << std::setw(15) << identity->machineName
                      << std::setw(8) << std::fixed << std::setprecision(1) << identity->getLoadPercentage()
                      << std::setw(10) << (identity->isHealthy() ? "Yes" : "No") << std::endl;
        }
    }
}

void ServiceMonitor::showServicesByMachine(const std::string& machine) const {
    const auto& registry = broker.getRegistry();
    auto serviceIds = registry.findServicesByMachine(machine);
    
    std::cout << "\n=== Services on machine: " << machine << " ===" << std::endl;
    
    if (serviceIds.empty()) {
        std::cout << "No services found on machine '" << machine << "'" << std::endl;
        return;
    }
    
    for (const auto& serviceId : serviceIds) {
        if (const auto* identity = registry.findServiceById(serviceId)) {
            std::cout << "  " << serviceId << " (" << identity->serviceName << ")"
                      << " - Load: " << identity->getLoadPercentage() << "%"
                      << " - Requests: " << identity->totalRequests
                      << " - Errors: " << identity->errorCount << std::endl;
        }
    }
}

void ServiceMonitor::showHealthStatus() const {
    const auto& registry = broker.getRegistry();
    auto healthyServices = registry.findHealthyServices();
    const auto allServices = registry.getAllServiceIds();
    
    std::cout << "\n=== Health Status ===" << std::endl;
    std::cout << "Total Services: " << allServices.size() << std::endl;
    std::cout << "Healthy Services: " << healthyServices.size() << std::endl;
    std::cout << "Unhealthy Services: " << (allServices.size() - healthyServices.size()) << std::endl;
    
    if (allServices.size() != healthyServices.size()) {
        std::cout << "\nUnhealthy Services:" << std::endl;
        std::set<std::string> healthySet(healthyServices.begin(), healthyServices.end());
        
        for (const auto& serviceId : allServices) {
            if (!healthySet.contains(serviceId)) {
                if (const auto* identity = registry.findServiceById(serviceId)) {
                    const auto secsSinceLastPing = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - identity->lastPing).count();
                    std::cout << "  " << serviceId << " - Last ping: " << secsSinceLastPing << "s ago" << std::endl;
                }
            }
        }
    }
}

void ServiceMonitor::clearScreen() {
    std::cout << "\033[2J\033[1;1H"; // ANSI escape sequence to clear screen
}

void ServiceMonitor::printHeader() {
    auto now = std::chrono::steady_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    
    std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                            SERVICE BROKER MONITOR                            ║" << std::endl;
    std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣" << std::endl;
    std::cout << "║ Time: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << std::string(50, ' ') << " ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝" << std::endl;
}

void ServiceMonitor::printBrokerStatus() const {
    auto status = broker.getBrokerStatus();
    
    std::cout << "\n📡 BROKER STATUS" << std::endl;
    std::cout << "┌─────────────────────────────────────────────────────────────────────────┐" << std::endl;
    std::cout << "│ Running: " << (status["running"].asBool() ? "✅ Yes" : "❌ No") << std::string(55, ' ') << "│" << std::endl;
    std::cout << "│ TCP Port: " << status["tcpPort"].asInt() << std::string(60, ' ') << "│" << std::endl;
    std::cout << "│ UNIX Socket: " << status["unixSocket"].asString() << std::string(45, ' ') << "│" << std::endl;
    std::cout << "│ Active Connections: " << status["activeConnections"].asInt() << std::string(50, ' ') << "│" << std::endl;
    std::cout << "│ Registered Services: " << status["registryStatus"]["totalServices"].asInt() << std::string(49, ' ') << "│" << std::endl;
    std::cout << "│ Healthy Services: " << status["registryStatus"]["healthyServices"].asInt() << std::string(52, ' ') << "│" << std::endl;
    std::cout << "└─────────────────────────────────────────────────────────────────────────┘" << std::endl;
}

void ServiceMonitor::printConnectionTable() const {
    auto connections = broker.getConnectionStatus();
    
    std::cout << "\nACTIVE CONNECTIONS" << std::endl;
    
    if (!connections.isArray() || connections.size() == 0) {
        std::cout << "   No active connections" << std::endl;
        return;
    }
    
    std::cout << "┌─────┬──────┬─────────────────────────────┬─────────────────┬──────────┬──────────┐" << std::endl;
    std::cout << "│ FD  │ Type │ Address                     │ Service ID      │ Status   │ Uptime   │" << std::endl;
    std::cout << "├─────┼──────┼─────────────────────────────┼─────────────────┼──────────┼──────────┤" << std::endl;
    
    for (const auto& conn : connections) {
        std::string address = conn["address"].asString();
        if (address.length() > 27) {
            address = "..." + address.substr(address.length() - 24);
        }
        
        std::string serviceId = conn["serviceId"].asString();
        if (serviceId.length() > 15) {
            serviceId = serviceId.substr(0, 12) + "...";
        }
        
        std::string status = conn["identified"].asBool() ? "✅ Ready" : "⏳ Pending";

        const auto uptimeSeconds = conn["uptimeSeconds"].asInt64();
        std::string uptime;
        if (uptimeSeconds < 60) {
            uptime = std::to_string(uptimeSeconds) + "s";
        } else if (uptimeSeconds < 3600) {
            uptime = std::to_string(uptimeSeconds / 60) + "m" + std::to_string(uptimeSeconds % 60) + "s";
        } else {
            const auto hours = uptimeSeconds / 3600;
            const auto minutes = (uptimeSeconds % 3600) / 60;
            uptime = std::to_string(hours) + "h" + std::to_string(minutes) + "m";
        }
        
        std::cout << "│ " << std::left << std::setw(3) << conn["fd"].asInt()
                  << " │ " << std::setw(4) << conn["type"].asString()
                  << " │ " << std::setw(27) << address
                  << " │ " << std::setw(15) << serviceId
                  << " │ " << std::setw(8) << status
                  << " │ " << std::setw(8) << uptime << " │" << std::endl;
    }
    
    std::cout << "└─────┴──────┴─────────────────────────────┴─────────────────┴──────────┴──────────┘" << std::endl;
}

void ServiceMonitor::printServiceTable() const {
    const auto& registry = broker.getRegistry();
    const auto services = registry.getAllServices();
    
    std::cout << "\nREGISTERED SERVICES" << std::endl;
    
    if (services.empty()) {
        std::cout << "   No registered services" << std::endl;
        return;
    }
    
    std::cout << "┌─────────────────┬─────────────────┬─────────────────┬────────┬─────────┬─────────┬─────────┐" << std::endl;
    std::cout << "│ Service ID      │ Name            │ Machine         │ Load % │ Reqs    │ Errors  │ Health  │" << std::endl;
    std::cout << "├─────────────────┼─────────────────┼─────────────────┼────────┼─────────┼─────────┼─────────┤" << std::endl;
    
    for (const auto& service : services) {
        std::string serviceId = service.serviceId;
        if (serviceId.length() > 15) serviceId = serviceId.substr(0, 12) + "...";
        
        std::string serviceName = service.serviceName;
        if (serviceName.length() > 15) serviceName = serviceName.substr(0, 12) + "...";
        
        std::string machineName = service.machineName;
        if (machineName.length() > 15) machineName = machineName.substr(0, 12) + "...";
        
        std::string healthStatus = service.isHealthy() ? "✅ OK" : "❌ BAD";
        
        std::cout << "│ " << std::left << std::setw(15) << serviceId
                  << " │ " << std::setw(15) << serviceName
                  << " │ " << std::setw(15) << machineName
                  << " │ " << std::right << std::setw(6) << std::fixed << std::setprecision(1) << service.getLoadPercentage()
                  << " │ " << std::setw(7) << service.totalRequests
                  << " │ " << std::setw(7) << service.errorCount
                  << " │ " << std::left << std::setw(7) << healthStatus << " │" << std::endl;
        
        // Show capabilities on next line
        if (!service.capabilities.empty()) {
            std::string caps = "Capabilities: ";
            for (size_t i = 0; i < service.capabilities.size(); ++i) {
                caps += service.capabilities[i];
                if (i < service.capabilities.size() - 1) caps += ", ";
            }
            if (caps.length() > 75) caps = caps.substr(0, 72) + "...";
            
            std::cout << "│ " << std::left << std::setw(87) << caps << " │" << std::endl;
            std::cout << "├─────────────────┼─────────────────┼─────────────────┼────────┼─────────┼─────────┼─────────┤" << std::endl;
        }
    }
    
    std::cout << "└─────────────────┴─────────────────┴─────────────────┴────────┴─────────┴─────────┴─────────┘" << std::endl;
}

void ServiceMonitor::printCapabilityIndex() const {
    const auto& registry = broker.getRegistry();
    const auto allServices = registry.getAllServices();
    
    // Build capability index
    std::map<std::string, std::vector<std::string>> capabilityMap;
    for (const auto& service : allServices) {
        for (const auto& capability : service.capabilities) {
            capabilityMap[capability].push_back(service.serviceId);
        }
    }
    
    std::cout << "\n🔍 CAPABILITY INDEX" << std::endl;
    
    if (capabilityMap.empty()) {
        std::cout << "   No capabilities registered" << std::endl;
        return;
    }
    
    std::cout << "┌──────────────────────┬─────────┬─────────────────────────────────────────────┐" << std::endl;
    std::cout << "│ Capability           │ Count   │ Service IDs                                 │" << std::endl;
    std::cout << "├──────────────────────┼─────────┼─────────────────────────────────────────────┤" << std::endl;
    
    for (const auto& [capability, serviceIds] : capabilityMap) {
        std::string cap = capability;
        if (cap.length() > 20) cap = cap.substr(0, 17) + "...";
        
        std::string ids;
        for (size_t i = 0; i < serviceIds.size(); ++i) {
            ids += serviceIds[i];
            if (i < serviceIds.size() - 1) ids += ", ";
        }
        if (ids.length() > 43) ids = ids.substr(0, 40) + "...";
        
        std::cout << "│ " << std::left << std::setw(20) << cap
                  << " │ " << std::right << std::setw(7) << serviceIds.size()
                  << " │ " << std::left << std::setw(43) << ids << " │" << std::endl;
    }
    
    std::cout << "└──────────────────────┴─────────┴─────────────────────────────────────────────┘" << std::endl;
}

} // namespace servicebroker