#pragma once

#include <limits>
#include <map>
#include <mutex>
#include <rapidjson/document.h>
#include <string>
#include <vector>

namespace rdws::metrics {

/// Per-capability accumulated statistics.
struct CapabilityStats {
  uint64_t requestCount = 0;
  uint64_t errorCount = 0;
  uint64_t timeoutCount = 0;
  double totalLatencyMs = 0.0;
  double minLatencyMs = std::numeric_limits<double>::max();
  double maxLatencyMs = 0.0;

  // Ring buffer of the last 200 latency samples — used for p99.
  static constexpr std::size_t kMaxSamples = 200;
  std::vector<double> recentLatencies;
};

/// Thread-safe tracker for per-capability request metrics.
class MetricsTracker {
public:
  /// Record one completed request.
  /// @param capability  Capability name (e.g. "echo").
  /// @param latencyMs   End-to-end HTTP latency in milliseconds.
  /// @param success     True if status 2xx, false on error.
  /// @param timedOut    True if the request timed out waiting for the service.
  void record(const std::string& capability, double latencyMs, bool success, bool timedOut = false);

  /// Serialize all accumulated stats to a RapidJSON document.
  rapidjson::Document toJson() const;

  /// Reset all counters (useful for testing).
  void reset();

private:
  mutable std::mutex mutex_;
  std::map<std::string, CapabilityStats> stats_;

  static double computePercentile(std::vector<double> samples, double pct);
};

} // namespace rdws::metrics
