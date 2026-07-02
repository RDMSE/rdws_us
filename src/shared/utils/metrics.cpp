#include "metrics.h"

#include "json_helper.h"

#include <algorithm>
#include <cmath>
#include <rapidjson/document.h>

namespace rdws::metrics {

void MetricsTracker::record(const std::string& capability, const double latencyMs,
                            const bool success, const bool timedOut) {
  std::scoped_lock lock(mutex_);
  auto& s = stats_[capability];

  s.requestCount++;
  if (!success) {
    s.errorCount++;
  }
  if (timedOut) {
    s.timeoutCount++;
  }

  s.totalLatencyMs += latencyMs;
  s.minLatencyMs = std::min(latencyMs, s.minLatencyMs);
  s.maxLatencyMs = std::max(latencyMs, s.maxLatencyMs);

  // Maintain ring buffer for percentile computation.
  if (s.recentLatencies.size() >= CapabilityStats::kMaxSamples) {
    s.recentLatencies.erase(s.recentLatencies.begin());
  }
  s.recentLatencies.push_back(latencyMs);
}

double MetricsTracker::computePercentile(std::vector<double> samples, const double pct) {
  if (samples.empty()) {
    return 0.0;
  }
  std::ranges::sort(samples.begin(), samples.end());
  const std::size_t idx =
      static_cast<std::size_t>(std::ceil(pct / 100.0 * static_cast<double>(samples.size()))) - 1;
  return samples[std::min(idx, samples.size() - 1)];
}

rapidjson::Value MetricsTracker::capabilityStatsToJson(const std::string& capability, const CapabilityStats& stats,
                                                        rapidjson::Document::AllocatorType& alloc) {
  const double avg = stats.requestCount > 0 ? stats.totalLatencyMs / static_cast<double>(stats.requestCount) : 0.0;
  const double p99 = computePercentile(stats.recentLatencies, 99.0);
  const double errorRate =
      stats.requestCount > 0 ? static_cast<double>(stats.errorCount) / static_cast<double>(stats.requestCount) : 0.0;
  const double minMs = stats.requestCount > 0 ? stats.minLatencyMs : 0.0;

  auto roundToDecimal = [](double value, int decimalPlaces) {
    const double factor = std::pow(10.0, decimalPlaces);
    return std::round(value * factor) / factor;
  };

  return rdws::utils::json::JsonObj(alloc)
      .set("capability", capability)
      .set("requestCount", stats.requestCount)
      .set("errorCount", stats.errorCount)
      .set("timeoutCount", stats.timeoutCount)
      .set("avgLatencyMs", roundToDecimal(avg, 2))
      .set("p99LatencyMs", roundToDecimal(p99, 2))
      .set("minLatencyMs", roundToDecimal(minMs, 2))
      .set("maxLatencyMs", roundToDecimal(stats.maxLatencyMs, 2))
      .set("errorRate", roundToDecimal(errorRate, 4))
      .take();
}

rapidjson::Document MetricsTracker::toJson() const {
  std::scoped_lock lock(mutex_);

  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();

  rapidjson::Value capabilities(rapidjson::kArrayType);
  for (const auto& [cap, capData] : stats_) {
    capabilities.PushBack(capabilityStatsToJson(cap, capData, alloc), alloc);
  }

  doc.AddMember("capabilities", capabilities, alloc);
  return doc;
}

void MetricsTracker::reset() {
  std::scoped_lock lock(mutex_);
  stats_.clear();
}

} // namespace rdws::metrics
