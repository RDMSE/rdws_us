#pragma once

#include "ServiceBroker.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace servicebroker {

class ServiceMonitor {
private:
    const ServiceBroker& broker;
    
    static void clearScreen() ;
    static void printHeader() ;
    void printBrokerStatus() const;
    void printConnectionTable() const;
    void printServiceTable() const;
    void printCapabilityIndex() const;
    
public:
    explicit ServiceMonitor(const ServiceBroker& serviceBroker);
    
    void displayStatus() const;
    void displayContinuous(int refreshIntervalSeconds = 5) const;
    void saveStatusToFile(const std::string& filename) const;
    
    // Specific queries
    void showServicesByCapability(const std::string& capability) const;
    void showServicesByMachine(const std::string& machine) const;
    void showHealthStatus() const;
};

} // namespace servicebroker