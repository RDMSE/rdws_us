#pragma once

#include "../Services/ServiceRegistry.h" // LoadBalancingStrategy

#include <chrono>
#include <optional>
#include <rapidjson/document.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace servicegateway {

// ─── Per-capability configuration ────────────────────────────────────────────

struct CapabilityConfig {
  /// Maximum time to wait for a service response.  0 = use global default.
  std::chrono::milliseconds timeoutMs{0};
  /// Load balancing strategy.  Set to std::nullopt to use global default.
  std::optional<LoadBalancingStrategy> loadBalancing;
  /// Maximum simultaneous in-flight requests for this capability.
  /// 0 = unlimited.
  int maxConcurrentRequests{0};
};

// ─── GatewayConfig ────────────────────────────────────────────────────────────

/// Thread-safe, dynamically updatable gateway configuration.
///
/// Supports:
///   - Global defaults for timeout and load balancing strategy.
///   - Per-capability overrides (timeout, LB, concurrency limit).
///   - Named feature flags (bool).
///   - Optional JSON file persistence (load on startup, save on change).
///
/// JSON file format:
/// @code
/// {
///   "defaults": {
///     "timeoutMs": 30000,
///     "loadBalancing": "least_loaded",
///     "maxConcurrentRequests": 0
///   },
///   "capabilities": {
///     "heavy-job": { "timeoutMs": 120000, "loadBalancing": "least_loaded" },
///     "greeting":  { "timeoutMs": 5000,   "loadBalancing": "round_robin" }
///   },
///   "features": {
///     "metrics_collection": true,
///     "request_logging":    true,
///     "rate_limiting":      false
///   }
/// }
/// @endcode
class GatewayConfig {
public:
  /// Construct with optional path for persistence.
  explicit GatewayConfig(std::string persistPath = "");

  // ── Factory ────────────────────────────────────────────────────────────
  /// Load from a JSON file.  Returns default config on any error (logs warning).
  static GatewayConfig fromFile(const std::string& path);

  // ── Lookup ─────────────────────────────────────────────────────────────
  /// Resolved config for a capability, merging per-cap overrides with defaults.
  [[nodiscard]] CapabilityConfig forCapability(const std::string& capability) const;

  /// Effective timeout for a capability (never zero — falls back to global default).
  [[nodiscard]] std::chrono::milliseconds timeoutFor(const std::string& capability) const;

  /// Effective load balancing strategy for a capability.
  [[nodiscard]] LoadBalancingStrategy lbStrategyFor(const std::string& capability) const;

  // ── Global defaults ────────────────────────────────────────────────────
  [[nodiscard]] CapabilityConfig defaults() const;
  void setDefaults(const CapabilityConfig& cfg);

  // ── Per-capability overrides ───────────────────────────────────────────
  void setCapabilityConfig(const std::string& capability, const CapabilityConfig& cfg);
  void removeCapabilityConfig(const std::string& capability);

  // ── Feature flags ──────────────────────────────────────────────────────
  [[nodiscard]] bool isEnabled(const std::string& feature, bool defaultValue = true) const;
  void setFeature(const std::string& feature, bool enabled);

  // ── Serialisation ──────────────────────────────────────────────────────
  [[nodiscard]] rapidjson::Document toJson() const;
  void loadFromJson(const rapidjson::Document& doc);

  // ── Persistence ────────────────────────────────────────────────────────
  [[nodiscard]] bool save() const;
  [[nodiscard]] bool hasPersistPath() const {
    return !persistPath_.empty();
  }

  // ── String ↔ enum helpers (public for use in HTTP handlers + tests) ────
  static std::string lbStrategyToString(LoadBalancingStrategy s);
  static LoadBalancingStrategy lbStrategyFromString(const std::string& s);

private:
  // unique_ptr keeps the mutex heap-allocated so GatewayConfig is movable.
  mutable std::unique_ptr<std::shared_mutex> mutex_ = std::make_unique<std::shared_mutex>();
  CapabilityConfig defaults_;
  std::unordered_map<std::string, CapabilityConfig> capabilities_;
  std::unordered_map<std::string, bool> features_;
  std::string persistPath_;
};

} // namespace servicegateway
