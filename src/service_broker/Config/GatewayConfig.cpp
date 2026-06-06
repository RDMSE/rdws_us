#include "GatewayConfig.h"

#include <fstream>
#include <iostream>

#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace servicegateway {

// ─── Construction ─────────────────────────────────────────────────────────────

GatewayConfig::GatewayConfig(std::string persistPath)
    : persistPath_(std::move(persistPath))
{
    // Sensible defaults
    defaults_.timeoutMs             = std::chrono::milliseconds(30000);
    defaults_.loadBalancing         = LoadBalancingStrategy::LEAST_LOADED;
    defaults_.maxConcurrentRequests = 0;

    // Default feature flags
    features_["metrics_collection"] = true;
    features_["request_logging"]    = true;
    features_["rate_limiting"]      = false;
}

// ─── Factory ──────────────────────────────────────────────────────────────────

GatewayConfig GatewayConfig::fromFile(const std::string &path)
{
    GatewayConfig cfg(path);

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[GatewayConfig] Config file not found: " << path
                  << " — using defaults\n";
        return cfg;
    }

    rapidjson::IStreamWrapper isw(file);
    rapidjson::Document doc;
    doc.ParseStream(isw);

    if (doc.HasParseError()) {
        std::cerr << "[GatewayConfig] Parse error in config file: " << path
                  << " — using defaults\n";
        return cfg;
    }

    cfg.loadFromJson(doc);
    return cfg;
}

// ─── Lookup ───────────────────────────────────────────────────────────────────

CapabilityConfig GatewayConfig::forCapability(const std::string &capability) const
{
    std::shared_lock lock(*mutex_);
    CapabilityConfig result = defaults_;

    const auto it = capabilities_.find(capability);
    if (it == capabilities_.end())
        return result;

    const CapabilityConfig &over = it->second;
    if (over.timeoutMs.count() > 0)        result.timeoutMs             = over.timeoutMs;
    if (over.loadBalancing.has_value())     result.loadBalancing         = over.loadBalancing;
    if (over.maxConcurrentRequests > 0)     result.maxConcurrentRequests = over.maxConcurrentRequests;
    return result;
}

std::chrono::milliseconds GatewayConfig::timeoutFor(const std::string &capability) const
{
    const auto cfg = forCapability(capability);
    if (cfg.timeoutMs.count() > 0) return cfg.timeoutMs;
    // Absolute fallback in case defaults were zeroed.
    return std::chrono::milliseconds(30000);
}

LoadBalancingStrategy GatewayConfig::lbStrategyFor(const std::string &capability) const
{
    const auto cfg = forCapability(capability);
    return cfg.loadBalancing.value_or(LoadBalancingStrategy::LEAST_LOADED);
}

// ─── Global defaults ──────────────────────────────────────────────────────────

CapabilityConfig GatewayConfig::defaults() const
{
    std::shared_lock lock(*mutex_);
    return defaults_;
}

void GatewayConfig::setDefaults(const CapabilityConfig &cfg)
{
    {
        std::unique_lock lock(*mutex_);
        defaults_ = cfg;
    }
    save();
}

// ─── Per-capability overrides ─────────────────────────────────────────────────

void GatewayConfig::setCapabilityConfig(const std::string &capability, const CapabilityConfig &cfg)
{
    {
        std::unique_lock lock(*mutex_);
        capabilities_[capability] = cfg;
    }
    save();
}

void GatewayConfig::removeCapabilityConfig(const std::string &capability)
{
    {
        std::unique_lock lock(*mutex_);
        capabilities_.erase(capability);
    }
    save();
}

// ─── Feature flags ────────────────────────────────────────────────────────────

bool GatewayConfig::isEnabled(const std::string &feature, const bool defaultValue) const
{
    std::shared_lock lock(*mutex_);
    const auto it = features_.find(feature);
    return it != features_.end() ? it->second : defaultValue;
}

void GatewayConfig::setFeature(const std::string &feature, const bool enabled)
{
    {
        std::unique_lock lock(*mutex_);
        features_[feature] = enabled;
    }
    save();
}

// ─── Serialisation ────────────────────────────────────────────────────────────

static rapidjson::Value capabilityConfigToJson(const CapabilityConfig &cfg,
                                               rapidjson::Document::AllocatorType &alloc)
{
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("timeoutMs",
                  static_cast<int64_t>(cfg.timeoutMs.count()),
                  alloc);
    if (cfg.loadBalancing.has_value()) {
        const std::string lb = GatewayConfig::lbStrategyToString(*cfg.loadBalancing);
        obj.AddMember("loadBalancing", rapidjson::Value(lb.c_str(), alloc), alloc);
    }
    obj.AddMember("maxConcurrentRequests", cfg.maxConcurrentRequests, alloc);
    return obj;
}

rapidjson::Document GatewayConfig::toJson() const
{
    std::shared_lock lock(*mutex_);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();

    // defaults
    doc.AddMember("defaults", capabilityConfigToJson(defaults_, alloc), alloc);

    // capabilities
    rapidjson::Value caps(rapidjson::kObjectType);
    for (const auto &[name, cfg] : capabilities_) {
        caps.AddMember(rapidjson::Value(name.c_str(), alloc),
                       capabilityConfigToJson(cfg, alloc),
                       alloc);
    }
    doc.AddMember("capabilities", caps, alloc);

    // features
    rapidjson::Value feats(rapidjson::kObjectType);
    for (const auto &[name, val] : features_) {
        feats.AddMember(rapidjson::Value(name.c_str(), alloc),
                        rapidjson::Value(val),
                        alloc);
    }
    doc.AddMember("features", feats, alloc);

    return doc;
}

void GatewayConfig::loadFromJson(const rapidjson::Document &doc)
{
    std::unique_lock lock(*mutex_);

    auto parseCapCfg = [](const rapidjson::Value &obj) -> CapabilityConfig {
        CapabilityConfig cfg;
        if (obj.HasMember("timeoutMs") && obj["timeoutMs"].IsInt64())
            cfg.timeoutMs = std::chrono::milliseconds(obj["timeoutMs"].GetInt64());
        else if (obj.HasMember("timeoutMs") && obj["timeoutMs"].IsInt())
            cfg.timeoutMs = std::chrono::milliseconds(obj["timeoutMs"].GetInt());
        if (obj.HasMember("loadBalancing") && obj["loadBalancing"].IsString())
            cfg.loadBalancing = GatewayConfig::lbStrategyFromString(obj["loadBalancing"].GetString());
        if (obj.HasMember("maxConcurrentRequests") && obj["maxConcurrentRequests"].IsInt())
            cfg.maxConcurrentRequests = obj["maxConcurrentRequests"].GetInt();
        return cfg;
    };

    if (doc.HasMember("defaults") && doc["defaults"].IsObject())
        defaults_ = parseCapCfg(doc["defaults"]);

    if (doc.HasMember("capabilities") && doc["capabilities"].IsObject()) {
        for (auto it = doc["capabilities"].MemberBegin();
             it != doc["capabilities"].MemberEnd(); ++it) {
            if (it->value.IsObject())
                capabilities_[it->name.GetString()] = parseCapCfg(it->value);
        }
    }

    if (doc.HasMember("features") && doc["features"].IsObject()) {
        for (auto it = doc["features"].MemberBegin();
             it != doc["features"].MemberEnd(); ++it) {
            if (it->value.IsBool())
                features_[it->name.GetString()] = it->value.GetBool();
        }
    }
}

// ─── Persistence ──────────────────────────────────────────────────────────────

bool GatewayConfig::save() const
{
    if (persistPath_.empty()) return true;

    rapidjson::Document doc = toJson();

    std::ofstream file(persistPath_);
    if (!file.is_open()) {
        std::cerr << "[GatewayConfig] Cannot write config file: " << persistPath_ << '\n';
        return false;
    }

    rapidjson::OStreamWrapper osw(file);
    rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);
    doc.Accept(writer);
    return true;
}

// ─── String ↔ enum helpers ────────────────────────────────────────────────────

std::string GatewayConfig::lbStrategyToString(const LoadBalancingStrategy s)
{
    switch (s) {
        case LoadBalancingStrategy::ROUND_ROBIN:      return "round_robin";
        case LoadBalancingStrategy::LEAST_LOADED:     return "least_loaded";
        case LoadBalancingStrategy::FASTEST_RESPONSE: return "fastest_response";
        case LoadBalancingStrategy::RANDOM:           return "random";
        default:                                      return "least_loaded";
    }
}

LoadBalancingStrategy GatewayConfig::lbStrategyFromString(const std::string &s)
{
    if (s == "round_robin")      return LoadBalancingStrategy::ROUND_ROBIN;
    if (s == "fastest_response") return LoadBalancingStrategy::FASTEST_RESPONSE;
    if (s == "random")           return LoadBalancingStrategy::RANDOM;
    return LoadBalancingStrategy::LEAST_LOADED; // default
}

} // namespace servicegateway
