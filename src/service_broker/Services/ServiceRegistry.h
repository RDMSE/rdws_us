#pragma once

#include "ServiceIdentity.h"
#include <map>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <rapidjson/document.h>

namespace servicegateway {

enum class LoadBalancingStrategy {
    ROUND_ROBIN,
    LEAST_LOADED,
    FASTEST_RESPONSE,
    RANDOM
};

class ServiceRegistry {
private:
    // Thread-safe access
    mutable std::mutex registryMutex;
    
    // Core registry
    std::map<std::string, ServiceIdentity> identities;
    
    // Indexes for fast lookup
    std::multimap<std::string, std::string> capabilityIndex;  // capability → serviceIds
    std::map<std::string, std::vector<std::string>> machineIndex; // machine → serviceIds
    std::map<std::string, std::vector<std::string>> environmentIndex; // environment → serviceIds
    
    // Round-robin state
    mutable std::map<std::string, size_t> roundRobinCounters;
    
    // Private helper methods
    void rebuildIndexes();
    void addToIndexes(const ServiceIdentity& identity);
    void removeFromIndexes(const ServiceIdentity& identity);
    
public:
    ServiceRegistry() = default;
    ~ServiceRegistry() = default;
    
    // Core operations
    bool registerService(const ServiceIdentity& identity);
    bool unregisterService(const std::string& serviceId);
    bool updateService(const ServiceIdentity& identity);
    
    // Lookup operations
    ServiceIdentity* findServiceById(const std::string& serviceId);
    const ServiceIdentity* findServiceById(const std::string& serviceId) const;
    
    std::vector<std::string> findServicesByCapability(const std::string& capability) const;
    std::vector<std::string> findServicesByMachine(const std::string& machine) const;
    std::vector<std::string> findServicesByEnvironment(const std::string& environment) const;
    std::vector<std::string> findServicesByName(const std::string& serviceName) const;
    
    // Advanced queries
    std::vector<std::string> findHealthyServices() const;
    std::vector<std::string> findAvailableServices(const std::string& capability) const; // healthy + not overloaded
    
    // Load balancing
    std::string selectBestService(const std::string& capability, 
                                 LoadBalancingStrategy strategy = LoadBalancingStrategy::LEAST_LOADED) const;
    
    // Statistics
    size_t getServiceCount() const;
    size_t getHealthyServiceCount() const;
    std::vector<std::string> getAllServiceIds() const;
    
    // Monitoring
    rapidjson::Document getRegistryStatus() const;
    std::vector<ServiceIdentity> getAllServices() const;
    
    // Maintenance operations
    void removeUnhealthyServices(std::chrono::seconds timeout = std::chrono::seconds(60));
    void updateServiceStats(const std::string& serviceId, uint32_t currentLoad, 
                          std::chrono::milliseconds responseTime);
    void recordServiceError(const std::string& serviceId);
    void pingService(const std::string& serviceId);
    
    // Custom filtering
    std::vector<std::string> findServices(const std::function<bool(const ServiceIdentity&)>& predicate) const;
};

} // namespace servicegateway