#include "GatewayConfig.h"

#include <fstream>
#include <iostream>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "utils/json_helper.h"

namespace json = rdws::utils::json;

namespace servicegateway {

// ─── Construction ─────────────────────────────────────────────────────────────

GatewayConfig::GatewayConfig(std::string persistPath) : persistPath_(std::move(persistPath)) {
  // Sensible defaults
  defaults_.timeoutMs = std::chrono::milliseconds(30000);
  defaults_.loadBalancing = LoadBalancingStrategy::LEAST_LOADED;
  defaults_.maxConcurrentRequests = 0;

  // Default feature flags
  features_["metrics_collection"] = true;
  features_["request_logging"] = true;
  features_["rate_limiting"] = false;
}

// ─── Factory ──────────────────────────────────────────────────────────────────

GatewayConfig GatewayConfig::fromFile(const std::string& path) {
  GatewayConfig cfg(path);

  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[GatewayConfig] Config file not found: " << path << " — using defaults\n";
    return cfg;
  }

  rapidjson::IStreamWrapper isw(file);
  rapidjson::Document doc;
  doc.ParseStream(isw);

  if (doc.HasParseError()) {
    std::cerr << "[GatewayConfig] Parse error in config file: " << path << " — using defaults\n";
    return cfg;
  }

  cfg.loadFromJson(doc);
  return cfg;
}

// ─── Lookup ───────────────────────────────────────────────────────────────────

CapabilityConfig GatewayConfig::forCapability(const std::string& capability) const {
  std::shared_lock lock(*mutex_);
  CapabilityConfig result = defaults_;

  const auto it = capabilities_.find(capability);
  if (it == capabilities_.end()) {
    return result;
  }

  const CapabilityConfig& over = it->second;
  if (over.timeoutMs.count() > 0) {
    result.timeoutMs = over.timeoutMs;
  }
  if (over.loadBalancing.has_value()) {
    result.loadBalancing = over.loadBalancing;
  }
  if (over.maxConcurrentRequests > 0) {
    result.maxConcurrentRequests = over.maxConcurrentRequests;
  }
  return result;
}

std::chrono::milliseconds GatewayConfig::timeoutFor(const std::string& capability) const {
  const auto cfg = forCapability(capability);
  if (cfg.timeoutMs.count() > 0) {
    return cfg.timeoutMs;
  }
  // Absolute fallback in case defaults were zeroed.
  return std::chrono::milliseconds(30000);
}

LoadBalancingStrategy GatewayConfig::lbStrategyFor(const std::string& capability) const {
  const auto cfg = forCapability(capability);
  return cfg.loadBalancing.value_or(LoadBalancingStrategy::LEAST_LOADED);
}

// ─── Global defaults ──────────────────────────────────────────────────────────

CapabilityConfig GatewayConfig::defaults() const {
  std::shared_lock lock(*mutex_);
  return defaults_;
}

void GatewayConfig::setDefaults(const CapabilityConfig& cfg) {
  {
    std::unique_lock lock(*mutex_);
    defaults_ = cfg;
  }
  save();
}

// ─── Per-capability overrides ─────────────────────────────────────────────────

void GatewayConfig::setCapabilityConfig(const std::string& capability,
                                        const CapabilityConfig& cfg) {
  {
    std::unique_lock lock(*mutex_);
    capabilities_[capability] = cfg;
  }
  save();
}

void GatewayConfig::removeCapabilityConfig(const std::string& capability) {
  {
    std::unique_lock lock(*mutex_);
    capabilities_.erase(capability);
  }
  save();
}

// ─── Feature flags ────────────────────────────────────────────────────────────

bool GatewayConfig::isEnabled(const std::string& feature, const bool defaultValue) const {
  std::shared_lock lock(*mutex_);
  const auto it = features_.find(feature);
  return it != features_.end() ? it->second : defaultValue;
}

void GatewayConfig::setFeature(const std::string& feature, const bool enabled) {
  {
    std::unique_lock lock(*mutex_);
    features_[feature] = enabled;
  }
  save();
}

// ─── Serialisation ────────────────────────────────────────────────────────────

static rapidjson::Value capabilityConfigToJson(const CapabilityConfig& cfg,
                                               rapidjson::Document::AllocatorType& alloc) {
  json::JsonObj obj(alloc);
  obj.set("timeoutMs", static_cast<int64_t>(cfg.timeoutMs.count()));
  if (cfg.loadBalancing.has_value()) {
    obj.set("loadBalancing", GatewayConfig::lbStrategyToString(*cfg.loadBalancing));
  }
  obj.set("maxConcurrentRequests", cfg.maxConcurrentRequests);
  return obj.take();
}

rapidjson::Document GatewayConfig::toJson() const {
  std::shared_lock lock(*mutex_);

  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();
  json::JsonObj obj(alloc);

  // defaults
  obj.setValue("defaults", capabilityConfigToJson(defaults_, alloc));

  // capabilities
  json::JsonObj caps(alloc);
  for (const auto& [name, cfg] : capabilities_) {
    caps.setValue(name, capabilityConfigToJson(cfg, alloc));
  }
  obj.setValue("capabilities", caps.take());

  // features
  json::JsonObj feats(alloc);
  for (const auto& [name, val] : features_) {
    feats.setValue(name, rapidjson::Value(val));
  }
  obj.setValue("features", feats.take());

  rapidjson::Value result = obj.take();
  doc.Swap(result);
  return doc;
}

void GatewayConfig::loadFromJson(const rapidjson::Document& doc) {
  std::unique_lock lock(*mutex_);

  auto parseCapCfg = [](const rapidjson::Value& obj) -> CapabilityConfig {
    auto getTimeoutMs = [&]() -> std::chrono::milliseconds {
      if (const auto val = json::getInt64(obj, "timeoutMs")) {
        return std::chrono::milliseconds(val.value());
      } else if (const auto val = json::getInt(obj, "timeoutMs")) {
        return std::chrono::milliseconds(val.value());
      }
      return std::chrono::milliseconds(0);
    };

    auto getLoadBalancing = [&]() -> std::optional<LoadBalancingStrategy> {
      if (const auto loadBalancingVal = json::getString(obj, "loadBalancing")) {
        return GatewayConfig::lbStrategyFromString(loadBalancingVal.value());
      }
      return std::nullopt;
    };

    return (CapabilityConfig){
      .timeoutMs = getTimeoutMs(),
      .loadBalancing = getLoadBalancing(),
      .maxConcurrentRequests = json::getInt(obj, "maxConcurrentRequests").value_or(0)
    };
  };

  if (const auto* defaults = json::getObject(doc, "defaults")) {
    defaults_ = parseCapCfg(*defaults);
  }

  if (const auto* capabilities = json::getObject(doc, "capabilities")) {
    for (auto it = capabilities->MemberBegin(); it != capabilities->MemberEnd(); ++it) {
      if (it->value.IsObject()) {
        capabilities_[it->name.GetString()] = parseCapCfg(it->value);
      }
    }
  }

  if (const auto* features = json::getObject(doc, "features")) {
    for (auto it = features->MemberBegin(); it != features->MemberEnd(); ++it) {
      if (it->value.IsBool()) {
        features_[it->name.GetString()] = it->value.GetBool();
      }
    }
  }
}

// ─── Persistence ──────────────────────────────────────────────────────────────

bool GatewayConfig::save() const {
  if (persistPath_.empty()) {
    return true;
  }

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

std::string GatewayConfig::lbStrategyToString(const LoadBalancingStrategy s) {
  switch (s) {
    case LoadBalancingStrategy::ROUND_ROBIN:
      return "round_robin";
    case LoadBalancingStrategy::LEAST_LOADED:
      return "least_loaded";
    case LoadBalancingStrategy::FASTEST_RESPONSE:
      return "fastest_response";
    case LoadBalancingStrategy::RANDOM:
      return "random";
    default:
      return "least_loaded";
  }
}

LoadBalancingStrategy GatewayConfig::lbStrategyFromString(const std::string& s) {
  if (s == "round_robin") {
    return LoadBalancingStrategy::ROUND_ROBIN;
  }
  if (s == "fastest_response") {
    return LoadBalancingStrategy::FASTEST_RESPONSE;
  }
  if (s == "random") {
    return LoadBalancingStrategy::RANDOM;
  }
  return LoadBalancingStrategy::LEAST_LOADED; // default
}

} // namespace servicegateway
