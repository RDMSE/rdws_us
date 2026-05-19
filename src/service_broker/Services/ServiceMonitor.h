#pragma once

#include "ServiceGateway.h"

namespace servicegateway {

class ServiceMonitor {
private:
    const ServiceGateway& gateway;
    
    static void clearScreen() ;
    static void printHeader() ;
    void printGatewayStatus() const;
    void printConnectionTable() const;
    void printServiceTable() const;
    void printCapabilityIndex() const;
    
public:
    explicit ServiceMonitor(const ServiceGateway& serviceGateway);
    
    void displayStatus() const;
    void displayContinuous(int refreshIntervalSeconds = 5) const;
    void saveStatusToFile(const std::string& filename) const;
    
    // Specific queries
    void showServicesByCapability(const std::string& capability) const;
    void showServicesByMachine(const std::string& machine) const;
    void showHealthStatus() const;
};

// Compatibility alias for phase-in rename Broker -> Gateway.
using ServiceGatewayMonitor = ServiceMonitor;

} // namespace servicegateway