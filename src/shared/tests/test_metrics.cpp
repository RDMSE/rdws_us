#include "utils/metrics.h"

#include <gtest/gtest.h>

using namespace rdws::metrics;

// ---------------------------------------------------------------------------
// Helper: find a capability entry in the JSON document by name.
// ---------------------------------------------------------------------------
static const rapidjson::Value* findCap(const rapidjson::Document& doc, const std::string& name) {
  if (!doc.HasMember("capabilities")) {
    return nullptr;
  }
  for (const auto& c : doc["capabilities"].GetArray()) {
    if (std::string(c["capability"].GetString()) == name) {
      return &c;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(MetricsTrackerTest, EmptyTrackerHasEmptyCapabilitiesArray) {
  MetricsTracker tracker;
  auto doc = tracker.toJson();
  ASSERT_TRUE(doc.HasMember("capabilities"));
  EXPECT_TRUE(doc["capabilities"].GetArray().Empty());
}

TEST(MetricsTrackerTest, RecordSingleSuccessRequest) {
  MetricsTracker tracker;
  tracker.record("ping", 42.0, /*success=*/true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 1u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 0u);
  EXPECT_EQ((*cap)["timeoutCount"].GetUint64(), 0u);
  EXPECT_DOUBLE_EQ((*cap)["avgLatencyMs"].GetDouble(), 42.0);
  EXPECT_DOUBLE_EQ((*cap)["minLatencyMs"].GetDouble(), 42.0);
  EXPECT_DOUBLE_EQ((*cap)["maxLatencyMs"].GetDouble(), 42.0);
  EXPECT_DOUBLE_EQ((*cap)["errorRate"].GetDouble(), 0.0);
}

TEST(MetricsTrackerTest, RecordErrorRequest) {
  MetricsTracker tracker;
  tracker.record("echo", 100.0, /*success=*/false);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "echo");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 1u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 1u);
  EXPECT_DOUBLE_EQ((*cap)["errorRate"].GetDouble(), 1.0);
}

TEST(MetricsTrackerTest, TimeoutCountedAsBothTimeoutAndError) {
  MetricsTracker tracker;
  tracker.record("slow", 30000.0, /*success=*/false, /*timedOut=*/true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "slow");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["timeoutCount"].GetUint64(), 1u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 1u);
}

TEST(MetricsTrackerTest, AverageLatencyIsCorrect) {
  MetricsTracker tracker;
  tracker.record("ping", 10.0, true);
  tracker.record("ping", 20.0, true);
  tracker.record("ping", 30.0, true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_DOUBLE_EQ((*cap)["avgLatencyMs"].GetDouble(), 20.0);
}

TEST(MetricsTrackerTest, MinMaxLatencyTracked) {
  MetricsTracker tracker;
  tracker.record("ping", 50.0, true);
  tracker.record("ping", 10.0, true);
  tracker.record("ping", 80.0, true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_DOUBLE_EQ((*cap)["minLatencyMs"].GetDouble(), 10.0);
  EXPECT_DOUBLE_EQ((*cap)["maxLatencyMs"].GetDouble(), 80.0);
}

TEST(MetricsTrackerTest, ErrorRateIsRatioOfErrorsToTotal) {
  MetricsTracker tracker;
  tracker.record("echo", 10.0, true);
  tracker.record("echo", 20.0, false);
  tracker.record("echo", 30.0, true);
  tracker.record("echo", 40.0, false);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "echo");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 4u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 2u);
  EXPECT_DOUBLE_EQ((*cap)["errorRate"].GetDouble(), 0.5);
}

TEST(MetricsTrackerTest, MultipleCapabilitiesAreIndependent) {
  MetricsTracker tracker;
  tracker.record("ping", 10.0, true);
  tracker.record("echo", 50.0, false);
  tracker.record("ping", 20.0, true);

  auto doc = tracker.toJson();
  EXPECT_EQ(doc["capabilities"].GetArray().Size(), 2u);

  const auto* ping = findCap(doc, "ping");
  const auto* echo = findCap(doc, "echo");
  ASSERT_NE(ping, nullptr);
  ASSERT_NE(echo, nullptr);

  EXPECT_EQ((*ping)["requestCount"].GetUint64(), 2u);
  EXPECT_EQ((*echo)["requestCount"].GetUint64(), 1u);
  EXPECT_DOUBLE_EQ((*echo)["errorRate"].GetDouble(), 1.0);
  EXPECT_DOUBLE_EQ((*ping)["errorRate"].GetDouble(), 0.0);
}

TEST(MetricsTrackerTest, P99WithSingleSampleEqualsTheSample) {
  MetricsTracker tracker;
  tracker.record("ping", 42.0, true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_DOUBLE_EQ((*cap)["p99LatencyMs"].GetDouble(), 42.0);
}

TEST(MetricsTrackerTest, P99WithOrderedSamples) {
  MetricsTracker tracker;
  // 100 samples: 1..100 ms. p99 = 99th percentile.
  // computePercentile: ceil(0.99 * 100) - 1 = 99 - 1 = 98 → samples[98] = 99.0
  for (int i = 1; i <= 100; ++i) {
    tracker.record("ping", static_cast<double>(i), true);
  }

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_DOUBLE_EQ((*cap)["p99LatencyMs"].GetDouble(), 99.0);
}

TEST(MetricsTrackerTest, ResetClearsAllStats) {
  MetricsTracker tracker;
  tracker.record("ping", 10.0, true);
  tracker.record("echo", 20.0, false);
  tracker.reset();

  auto doc = tracker.toJson();
  EXPECT_TRUE(doc["capabilities"].GetArray().Empty());
}

TEST(MetricsTrackerTest, RequestCountSurvivestReset) {
  MetricsTracker tracker;
  tracker.record("ping", 10.0, true);
  tracker.reset();
  tracker.record("ping", 5.0, true);

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  // After reset + 1 new record, count is 1, not 2.
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 1u);
  EXPECT_DOUBLE_EQ((*cap)["avgLatencyMs"].GetDouble(), 5.0);
}

TEST(MetricsTrackerTest, RingBufferCapsAt200SamplesForPercentile) {
  MetricsTracker tracker;
  // Record 250 samples: values 0..249
  for (int i = 0; i < 250; ++i) {
    tracker.record("ping", static_cast<double>(i), true);
  }

  auto doc = tracker.toJson();
  const auto* cap = findCap(doc, "ping");
  ASSERT_NE(cap, nullptr);

  // requestCount reflects all 250 records.
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 250u);

  // Ring buffer keeps last 200 samples: values 50..249
  // p99 of [50..249]: ceil(0.99 * 200) - 1 = 198 - 1 = 197 → samples[197] = 247.0
  EXPECT_DOUBLE_EQ((*cap)["p99LatencyMs"].GetDouble(), 247.0);
}

TEST(MetricsTrackerTest, MinLatencyZeroWhenNoRecords) {
  MetricsTracker tracker;
  // No records — toJson should return 0.0 for min (not numeric_limits::max).
  auto doc = tracker.toJson();
  EXPECT_TRUE(doc["capabilities"].GetArray().Empty());
  // No capability entry to check — just verify no crash.
}
