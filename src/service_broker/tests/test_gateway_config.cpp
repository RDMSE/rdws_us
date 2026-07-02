#include "../Config/GatewayConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "../utils/json_helper.h"

namespace servicegateway {

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string docToString(const rapidjson::Document& doc) {
  return rdws::utils::json::docToString(doc);
}

static std::string tmpPath() {
  return (std::filesystem::temp_directory_path() / "rdws_test_config.json").string();
}

// ─── Default construction ─────────────────────────────────────────────────────

TEST(GatewayConfigDefaults, DefaultTimeoutIs30s) {
  GatewayConfig cfg;
  EXPECT_EQ(cfg.timeoutFor("any-cap"), 30000ms);
}

TEST(GatewayConfigDefaults, DefaultLbStrategy_IsLeastLoaded) {
  GatewayConfig cfg;
  EXPECT_EQ(cfg.lbStrategyFor("any-cap"), LoadBalancingStrategy::LEAST_LOADED);
}

TEST(GatewayConfigDefaults, DefaultFeatureFlags_MetricsEnabled) {
  GatewayConfig cfg;
  EXPECT_TRUE(cfg.isEnabled("metrics_collection"));
  EXPECT_TRUE(cfg.isEnabled("request_logging"));
  EXPECT_FALSE(cfg.isEnabled("rate_limiting"));
}

TEST(GatewayConfigDefaults, UnknownFeature_ReturnsDefaultValue) {
  GatewayConfig cfg;
  EXPECT_TRUE(cfg.isEnabled("nonexistent_feature", true));
  EXPECT_FALSE(cfg.isEnabled("nonexistent_feature", false));
}

// ─── Per-capability overrides ─────────────────────────────────────────────────

TEST(GatewayConfigCapability, SetTimeout_OverridesDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.timeoutMs = 5000ms;
  cfg.setCapabilityConfig("greeting", cap);
  EXPECT_EQ(cfg.timeoutFor("greeting"), 5000ms);
}

TEST(GatewayConfigCapability, UnsetCapability_FallsBackToDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.timeoutMs = 5000ms;
  cfg.setCapabilityConfig("greeting", cap);
  // "other" has no override — should use default 30s.
  EXPECT_EQ(cfg.timeoutFor("other"), 30000ms);
}

TEST(GatewayConfigCapability, SetLbStrategy_OverridesDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.loadBalancing = LoadBalancingStrategy::ROUND_ROBIN;
  cfg.setCapabilityConfig("fast-cap", cap);
  EXPECT_EQ(cfg.lbStrategyFor("fast-cap"), LoadBalancingStrategy::ROUND_ROBIN);
}

TEST(GatewayConfigCapability, RemoveCapability_RevertsToDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.timeoutMs = 1000ms;
  cfg.setCapabilityConfig("greeting", cap);
  cfg.removeCapabilityConfig("greeting");
  EXPECT_EQ(cfg.timeoutFor("greeting"), 30000ms);
}

TEST(GatewayConfigCapability, PartialOverride_ZeroTimeout_DoesNotOverrideDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.timeoutMs = 0ms; // 0 = "not set"
  cap.loadBalancing = LoadBalancingStrategy::RANDOM;
  cfg.setCapabilityConfig("cap", cap);
  // timeout should still be default
  EXPECT_EQ(cfg.timeoutFor("cap"), 30000ms);
  // LB should be overridden
  EXPECT_EQ(cfg.lbStrategyFor("cap"), LoadBalancingStrategy::RANDOM);
}

TEST(GatewayConfigCapability, MaxConcurrentRequests_ZeroMeansUnlimited) {
  GatewayConfig cfg;
  const CapabilityConfig resolved = cfg.forCapability("any");
  EXPECT_EQ(resolved.maxConcurrentRequests, 0);
}

TEST(GatewayConfigCapability, SetMaxConcurrent_OverridesDefault) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.maxConcurrentRequests = 10;
  cfg.setCapabilityConfig("heavy", cap);
  EXPECT_EQ(cfg.forCapability("heavy").maxConcurrentRequests, 10);
}

// ─── Feature flags ────────────────────────────────────────────────────────────

TEST(GatewayConfigFeatures, SetFeature_EnablesFlag) {
  GatewayConfig cfg;
  cfg.setFeature("rate_limiting", true);
  EXPECT_TRUE(cfg.isEnabled("rate_limiting"));
}

TEST(GatewayConfigFeatures, SetFeature_DisablesFlag) {
  GatewayConfig cfg;
  cfg.setFeature("metrics_collection", false);
  EXPECT_FALSE(cfg.isEnabled("metrics_collection"));
}

// ─── Serialisation ────────────────────────────────────────────────────────────

TEST(GatewayConfigJson, ToJson_ContainsDefaults) {
  GatewayConfig cfg;
  const rapidjson::Document doc = cfg.toJson();
  ASSERT_TRUE(doc.HasMember("defaults"));
  EXPECT_TRUE(doc["defaults"].HasMember("timeoutMs"));
}

TEST(GatewayConfigJson, ToJson_ContainsFeatures) {
  GatewayConfig cfg;
  const rapidjson::Document doc = cfg.toJson();
  ASSERT_TRUE(doc.HasMember("features"));
  EXPECT_TRUE(doc["features"].HasMember("metrics_collection"));
}

TEST(GatewayConfigJson, ToJson_ContainsCapabilities) {
  GatewayConfig cfg;
  CapabilityConfig cap;
  cap.timeoutMs = 7000ms;
  cfg.setCapabilityConfig("my-cap", cap);

  const rapidjson::Document doc = cfg.toJson();
  ASSERT_TRUE(doc.HasMember("capabilities"));
  ASSERT_TRUE(doc["capabilities"].HasMember("my-cap"));
  EXPECT_EQ(doc["capabilities"]["my-cap"]["timeoutMs"].GetInt64(), 7000);
}

TEST(GatewayConfigJson, LoadFromJson_RoundTrip) {
  GatewayConfig original;
  CapabilityConfig cap;
  cap.timeoutMs = 9000ms;
  cap.loadBalancing = LoadBalancingStrategy::ROUND_ROBIN;
  original.setCapabilityConfig("svc", cap);
  original.setFeature("rate_limiting", true);

  const rapidjson::Document doc = original.toJson();

  GatewayConfig restored;
  restored.loadFromJson(doc);

  EXPECT_EQ(restored.timeoutFor("svc"), 9000ms);
  EXPECT_EQ(restored.lbStrategyFor("svc"), LoadBalancingStrategy::ROUND_ROBIN);
  EXPECT_TRUE(restored.isEnabled("rate_limiting"));
}

// ─── File persistence ─────────────────────────────────────────────────────────

TEST(GatewayConfigFile, SaveAndLoad_RoundTrip) {
  const std::string path = tmpPath();

  GatewayConfig original(path);
  CapabilityConfig cap;
  cap.timeoutMs = 12000ms;
  cap.loadBalancing = LoadBalancingStrategy::FASTEST_RESPONSE;
  original.setCapabilityConfig("analytics", cap);
  original.setFeature("request_logging", false);
  ASSERT_TRUE(original.save());

  GatewayConfig loaded = GatewayConfig::fromFile(path);
  EXPECT_EQ(loaded.timeoutFor("analytics"), 12000ms);
  EXPECT_EQ(loaded.lbStrategyFor("analytics"), LoadBalancingStrategy::FASTEST_RESPONSE);
  EXPECT_FALSE(loaded.isEnabled("request_logging"));

  std::filesystem::remove(path);
}

TEST(GatewayConfigFile, FromFile_NonExistent_UsesDefaults) {
  GatewayConfig cfg = GatewayConfig::fromFile("/tmp/rdws_does_not_exist_12345.json");
  EXPECT_EQ(cfg.timeoutFor("x"), 30000ms);
}

// ─── String conversions ───────────────────────────────────────────────────────

TEST(GatewayConfigLbString, RoundTrip_AllStrategies) {
  const std::vector<LoadBalancingStrategy> strategies = {
      LoadBalancingStrategy::ROUND_ROBIN,
      LoadBalancingStrategy::LEAST_LOADED,
      LoadBalancingStrategy::FASTEST_RESPONSE,
      LoadBalancingStrategy::RANDOM,
  };
  for (const auto s : strategies) {
    const std::string str = GatewayConfig::lbStrategyToString(s);
    EXPECT_EQ(GatewayConfig::lbStrategyFromString(str), s);
  }
}

TEST(GatewayConfigLbString, UnknownString_DefaultsToLeastLoaded) {
  EXPECT_EQ(GatewayConfig::lbStrategyFromString("nonexistent"),
            LoadBalancingStrategy::LEAST_LOADED);
}

// ─── setDefaults propagation ──────────────────────────────────────────────────

TEST(GatewayConfigDefaults, SetDefaults_PropagatesGlobally) {
  GatewayConfig cfg;
  CapabilityConfig newDefaults;
  newDefaults.timeoutMs = 60000ms;
  newDefaults.loadBalancing = LoadBalancingStrategy::RANDOM;
  cfg.setDefaults(newDefaults);

  // All capabilities without override should use the new default.
  EXPECT_EQ(cfg.timeoutFor("anything"), 60000ms);
  EXPECT_EQ(cfg.lbStrategyFor("anything"), LoadBalancingStrategy::RANDOM);
}

} // namespace servicegateway
