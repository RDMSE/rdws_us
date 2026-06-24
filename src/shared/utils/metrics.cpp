#include "metrics.h"

#include <algorithm>
#include <cmath>

#include <rapidjson/document.h>

namespace rdws::metrics {

void MetricsTracker::record(const std::string &capability,
                            const double latencyMs,
                            const bool success,
                            const bool timedOut)
{
    std::scoped_lock lock(mutex_);
    auto &s = stats_[capability];

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

double MetricsTracker::computePercentile(std::vector<double> samples, const double pct)
{
    if (samples.empty()) {
      return 0.0;
    }
    std::ranges::sort(samples.begin(), samples.end());
    const std::size_t idx = static_cast<std::size_t>(
        std::ceil(pct / 100.0 * static_cast<double>(samples.size()))) - 1;
    return samples[std::min(idx, samples.size() - 1)];
}

rapidjson::Document MetricsTracker::toJson() const
{
    std::scoped_lock lock(mutex_);

    rapidjson::Document doc;
    doc.SetObject();
    auto &alloc = doc.GetAllocator();

    rapidjson::Value capabilities(rapidjson::kArrayType);

    for (const auto &[cap, s] : stats_) {
        const double avg = s.requestCount > 0
            ? s.totalLatencyMs / static_cast<double>(s.requestCount)
            : 0.0;
        const double p99 = computePercentile(s.recentLatencies, 99.0);
        const double errorRate = s.requestCount > 0
            ? static_cast<double>(s.errorCount) / static_cast<double>(s.requestCount)
            : 0.0;
        const double minMs = s.requestCount > 0 ? s.minLatencyMs : 0.0;

        rapidjson::Value entry(rapidjson::kObjectType);
        entry.AddMember("capability",    rapidjson::Value(cap.c_str(), alloc), alloc);
        entry.AddMember("requestCount",  static_cast<uint64_t>(s.requestCount),  alloc);
        entry.AddMember("errorCount",    static_cast<uint64_t>(s.errorCount),    alloc);
        entry.AddMember("timeoutCount",  static_cast<uint64_t>(s.timeoutCount),  alloc);
        entry.AddMember("avgLatencyMs",  std::round(avg  * 100.0) / 100.0, alloc);
        entry.AddMember("p99LatencyMs",  std::round(p99  * 100.0) / 100.0, alloc);
        entry.AddMember("minLatencyMs",  std::round(minMs * 100.0) / 100.0, alloc);
        entry.AddMember("maxLatencyMs",  std::round(s.maxLatencyMs * 100.0) / 100.0, alloc);
        entry.AddMember("errorRate",     std::round(errorRate * 10000.0) / 10000.0, alloc);

        capabilities.PushBack(entry, alloc);
    }

    doc.AddMember("capabilities", capabilities, alloc);
    return doc;
}

void MetricsTracker::reset()
{
    std::scoped_lock lock(mutex_);
    stats_.clear();
}

} // namespace rdws::metrics
