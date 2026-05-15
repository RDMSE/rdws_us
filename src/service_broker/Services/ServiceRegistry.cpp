#include "ServiceRegistry.h"
#include <algorithm>
#include <random>
#include <chrono>
#include <iostream>

namespace servicegateway
{

    bool ServiceRegistry::registerService(const ServiceIdentity &identity)
    {
        std::lock_guard lock(registryMutex);

        if (identity.serviceId.empty())
        {
            std::cerr << "Cannot register service with empty serviceId" << '\n';
            return false;
        }

        // Check if service already exists
        if (identities.find(identity.serviceId) != identities.end())
        {
            std::cout << "Service " << identity.serviceId << " already registered, updating..." << '\n';
            return updateService(identity);
        }

        // Add to main registry
        identities[identity.serviceId] = identity;

        // Add to indexes
        addToIndexes(identity);

        std::cout << "Registered service: " << identity.serviceId
                  << " (" << identity.serviceName << ") from " << identity.machineName << '\n';

        return true;
    }

    bool ServiceRegistry::unregisterService(const std::string &serviceId)
    {
        std::lock_guard lock(registryMutex);

        const auto it = identities.find(serviceId);
        if (it == identities.end())
        {
            return false;
        }

        // Remove from indexes
        removeFromIndexes(it->second);

        // Remove from main registry
        identities.erase(it);

        std::cout << "Unregistered service: " << serviceId << '\n';
        return true;
    }

    bool ServiceRegistry::updateService(const ServiceIdentity &identity)
    {
        std::lock_guard lock(registryMutex);

        const auto it = identities.find(identity.serviceId);
        if (it == identities.end())
        {
            return false;
        }

        // Remove old indexes
        removeFromIndexes(it->second);

        // Update identity
        it->second = identity;

        // Rebuild indexes for this service
        addToIndexes(identity);

        return true;
    }

    ServiceIdentity *ServiceRegistry::findServiceById(const std::string &serviceId)
    {
        std::lock_guard lock(registryMutex);
        const auto it = identities.find(serviceId);
        return (it != identities.end()) ? &it->second : nullptr;
    }

    const ServiceIdentity *ServiceRegistry::findServiceById(const std::string &serviceId) const
    {
        std::lock_guard lock(registryMutex);
        const auto it = identities.find(serviceId);
        return (it != identities.end()) ? &it->second : nullptr;
    }

    std::vector<std::string> ServiceRegistry::findServicesByCapability(const std::string &capability) const
    {
        std::lock_guard<std::mutex> lock(registryMutex);

        std::vector<std::string> result;
        auto [fst, snd] = capabilityIndex.equal_range(capability);

        for (auto it = fst; it != snd; ++it)
        {
            result.push_back(it->second);
        }

        return result;
    }

    std::vector<std::string> ServiceRegistry::findServicesByMachine(const std::string &machine) const
    {
        std::lock_guard<std::mutex> lock(registryMutex);

        if (const auto it = machineIndex.find(machine); it != machineIndex.end())
        {
            return it->second;
        }
        return {};
    }

    std::vector<std::string> ServiceRegistry::findServicesByEnvironment(const std::string &environment) const
    {
        std::lock_guard<std::mutex> lock(registryMutex);

        if (const auto it = environmentIndex.find(environment); it != environmentIndex.end())
        {
            return it->second;
        }
        return {};
    }

    std::vector<std::string> ServiceRegistry::findServicesByName(const std::string &serviceName) const
    {
        std::lock_guard lock(registryMutex);

        std::vector<std::string> result;
        for (const auto &[serviceId, identity] : identities)
        {
            if (identity.serviceName == serviceName)
            {
                result.push_back(serviceId);
            }
        }
        return result;
    }

    std::vector<std::string> ServiceRegistry::findHealthyServices() const
    {
        return findServices([](const ServiceIdentity &identity)
                            { return identity.isHealthy(); });
    }

    std::vector<std::string> ServiceRegistry::findAvailableServices(const std::string &capability) const
    {
        std::lock_guard lock(registryMutex);

        std::vector<std::string> result;
        auto [fst, snd] = capabilityIndex.equal_range(capability);

        for (auto it = fst; it != snd; ++it)
        {
            const auto &serviceId = it->second;
            if (auto it = identities.find(serviceId); it != identities.end() && it->second.isHealthy() && !it->second.isOverloaded())
            {
                result.push_back(serviceId);
            }
        }

        return result;
    }

    std::string ServiceRegistry::selectBestService(const std::string &capability,
                                                   LoadBalancingStrategy strategy) const
    {
        auto availableServices = findAvailableServices(capability);

        if (availableServices.empty())
        {
            return "";
        }

        std::lock_guard lock(registryMutex);

        switch (strategy)
        {
        case LoadBalancingStrategy::ROUND_ROBIN:
        {
            auto &counter = roundRobinCounters[capability];
            auto selectedId = availableServices[counter % availableServices.size()];
            counter++;
            return selectedId;
        }

        case LoadBalancingStrategy::LEAST_LOADED:
        {
            std::string bestService = availableServices[0];
            double minLoad = 100.0;

            for (const auto &serviceId : availableServices)
            {
                if (auto it = identities.find(serviceId); it != identities.end())
                {
                    if (double load = it->second.getLoadPercentage(); load < minLoad)
                    {
                        minLoad = load;
                        bestService = serviceId;
                    }
                }
            }
            return bestService;
        }

        case LoadBalancingStrategy::FASTEST_RESPONSE:
        {
            std::string fastestService = availableServices[0];
            auto minResponseTime = std::chrono::milliseconds::max();

            for (const auto &serviceId : availableServices)
            {
                auto it = identities.find(serviceId);
                if (it != identities.end())
                {
                    if (it->second.avgResponseTime < minResponseTime)
                    {
                        minResponseTime = it->second.avgResponseTime;
                        fastestService = serviceId;
                    }
                }
            }
            return fastestService;
        }

        case LoadBalancingStrategy::RANDOM:
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, availableServices.size() - 1);
            return availableServices[dis(gen)];
        }
        }

        return availableServices[0]; // Fallback
    }

    size_t ServiceRegistry::getServiceCount() const
    {
        std::lock_guard lock(registryMutex);
        return identities.size();
    }

    size_t ServiceRegistry::getHealthyServiceCount() const
    {
        return findHealthyServices().size();
    }

    std::vector<std::string> ServiceRegistry::getAllServiceIds() const
    {
        std::lock_guard lock(registryMutex);

        std::vector<std::string> result;
        for (const auto &[serviceId, identity] : identities)
        {
            result.push_back(serviceId);
        }
        return result;
    }

    rapidjson::Document ServiceRegistry::getRegistryStatus() const
    {
        std::lock_guard lock(registryMutex);

        rapidjson::Document status;
        status.SetObject();
        auto &allocator = status.GetAllocator();

        size_t healthyCount = 0;
        for (const auto &[serviceId, identity] : identities)
        {
            if (identity.isHealthy())
            {
                ++healthyCount;
            }
        }

        status.AddMember("totalServices", static_cast<int>(identities.size()), allocator);
        status.AddMember("healthyServices", static_cast<int>(healthyCount), allocator);

        rapidjson::Value servicesArray(rapidjson::kArrayType);
        for (const auto &[serviceId, identity] : identities)
        {
            servicesArray.PushBack(identity.toJsonValue(allocator), allocator);
        }
        status.AddMember("services", servicesArray, allocator);

        return status;
    }

    std::vector<ServiceIdentity> ServiceRegistry::getAllServices() const
    {
        std::lock_guard lock(registryMutex);

        std::vector<ServiceIdentity> result;
        for (const auto &[serviceId, identity] : identities)
        {
            result.push_back(identity);
        }
        return result;
    }

    void ServiceRegistry::removeUnhealthyServices(std::chrono::seconds timeout)
    {
        std::lock_guard lock(registryMutex);

        std::vector<std::string> toRemove;
        for (const auto &[serviceId, identity] : identities)
        {
            if (!identity.isHealthy(timeout))
            {
                toRemove.push_back(serviceId);
            }
        }

        for (const auto &serviceId : toRemove)
        {
            std::cout << "Removing unhealthy service: " << serviceId << '\n';
            unregisterService(serviceId);
        }
    }

    void ServiceRegistry::updateServiceStats(const std::string &serviceId, uint32_t currentLoad,
                                             std::chrono::milliseconds responseTime)
    {
        std::lock_guard<std::mutex> lock(registryMutex);

        if (const auto it = identities.find(serviceId); it != identities.end())
        {
            it->second.currentLoad = currentLoad;
            it->second.totalRequests++;

            // Update average response time (simple moving average)
            if (it->second.totalRequests == 1)
            {
                it->second.avgResponseTime = responseTime;
            }
            else
            {
                const auto avgMs = it->second.avgResponseTime.count();
                const auto newMs = responseTime.count();
                const auto newAvg = (static_cast<double>(avgMs) * 0.9) + (static_cast<double>(newMs) * 0.1); // Weighted average
                it->second.avgResponseTime = std::chrono::milliseconds(static_cast<long>(newAvg));
            }
        }
    }

    void ServiceRegistry::recordServiceError(const std::string &serviceId)
    {
        std::lock_guard lock(registryMutex);

        auto it = identities.find(serviceId);
        if (it != identities.end())
        {
            it->second.errorCount++;
        }
    }

    void ServiceRegistry::pingService(const std::string &serviceId)
    {
        std::lock_guard lock(registryMutex);

        auto it = identities.find(serviceId);
        if (it != identities.end())
        {
            it->second.lastPing = std::chrono::steady_clock::now();
        }
    }

    std::vector<std::string> ServiceRegistry::findServices(const std::function<bool(const ServiceIdentity &)> &predicate) const
    {
        std::lock_guard lock(registryMutex);

        std::vector<std::string> result;
        for (const auto &[serviceId, identity] : identities)
        {
            if (predicate(identity))
            {
                result.push_back(serviceId);
            }
        }
        return result;
    }

    // Private helper methods
    void ServiceRegistry::addToIndexes(const ServiceIdentity &identity)
    {
        // Add to capability index
        for (const auto &capability : identity.capabilities)
        {
            capabilityIndex.insert({capability, identity.serviceId});
        }

        // Add to machine index
        machineIndex[identity.machineName].push_back(identity.serviceId);

        // Add to environment index
        environmentIndex[identity.environment].push_back(identity.serviceId);
    }

    void ServiceRegistry::removeFromIndexes(const ServiceIdentity &identity)
    {
        // Remove from capability index
        for (const auto &capability : identity.capabilities)
        {
            auto [fst, snd] = capabilityIndex.equal_range(capability);
            for (auto it = fst; it != snd; ++it)
            {
                if (it->second == identity.serviceId)
                {
                    capabilityIndex.erase(it);
                    break;
                }
            }
        }

        // Remove from machine index
        auto &machineServices = machineIndex[identity.machineName];
        machineServices.erase(
            std::remove(machineServices.begin(), machineServices.end(), identity.serviceId),
            machineServices.end());

        // Remove from environment index
        auto &envServices = environmentIndex[identity.environment];
        envServices.erase(
            std::remove(envServices.begin(), envServices.end(), identity.serviceId),
            envServices.end());
    }

} // namespace servicegateway