#include "Services/ServiceGateway.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>

using namespace servicegateway;

// ---------------------------------------------------------------------------
// Helper: find a capability entry by name in a /metrics JSON document.
// ---------------------------------------------------------------------------
static const rapidjson::Value* findMetricsCap(const rapidjson::Document& doc,
                                              const std::string& name) {
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
// ServiceGateway::getMetrics() tests
// ---------------------------------------------------------------------------

TEST(ServiceGatewayMetricsTest, FreshGatewayHasEmptyCapabilities) {
  ServiceGateway gw(19000, "/tmp/rdws_gw_test_metrics.sock");

  auto doc = gw.getMetrics();
  ASSERT_TRUE(doc.HasMember("capabilities"));
  EXPECT_TRUE(doc["capabilities"].GetArray().Empty());
}

TEST(ServiceGatewayMetricsTest, RecordMetricAppearsInGetMetrics) {
  ServiceGateway gw(19001, "/tmp/rdws_gw_test_metrics2.sock");

  gw.recordMetric("ping", 50.0, /*success=*/true);

  auto doc = gw.getMetrics();
  const auto* cap = findMetricsCap(doc, "ping");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["requestCount"].GetUint64(), 1u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 0u);
  EXPECT_DOUBLE_EQ((*cap)["avgLatencyMs"].GetDouble(), 50.0);
  EXPECT_DOUBLE_EQ((*cap)["errorRate"].GetDouble(), 0.0);
}

TEST(ServiceGatewayMetricsTest, RecordErrorMetricUpdatesErrorCount) {
  ServiceGateway gw(19002, "/tmp/rdws_gw_test_metrics3.sock");

  gw.recordMetric("echo", 100.0, /*success=*/false);

  auto doc = gw.getMetrics();
  const auto* cap = findMetricsCap(doc, "echo");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 1u);
  EXPECT_DOUBLE_EQ((*cap)["errorRate"].GetDouble(), 1.0);
}

TEST(ServiceGatewayMetricsTest, RecordTimeoutMetricUpdatesTimeoutCount) {
  ServiceGateway gw(19003, "/tmp/rdws_gw_test_metrics4.sock");

  gw.recordMetric("slow", 30000.0, /*success=*/false, /*timedOut=*/true);

  auto doc = gw.getMetrics();
  const auto* cap = findMetricsCap(doc, "slow");
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ((*cap)["timeoutCount"].GetUint64(), 1u);
  EXPECT_EQ((*cap)["errorCount"].GetUint64(), 1u);
}

TEST(ServiceGatewayMetricsTest, MultipleCapabilitiesTrackedSeparately) {
  ServiceGateway gw(19004, "/tmp/rdws_gw_test_metrics5.sock");

  gw.recordMetric("ping", 10.0, true);
  gw.recordMetric("echo", 20.0, true);
  gw.recordMetric("ping", 30.0, false);

  auto doc = gw.getMetrics();
  EXPECT_EQ(doc["capabilities"].GetArray().Size(), 2u);

  const auto* ping = findMetricsCap(doc, "ping");
  const auto* echo = findMetricsCap(doc, "echo");
  ASSERT_NE(ping, nullptr);
  ASSERT_NE(echo, nullptr);
  EXPECT_EQ((*ping)["requestCount"].GetUint64(), 2u);
  EXPECT_EQ((*echo)["requestCount"].GetUint64(), 1u);
}

// ---------------------------------------------------------------------------
// ServiceGateway::getHealth() tests
// ---------------------------------------------------------------------------

TEST(ServiceGatewayHealthTest, FreshGatewayHealthHasRequiredTopLevelFields) {
  ServiceGateway gw(19010, "/tmp/rdws_gw_test_health1.sock");

  auto doc = gw.getHealth();

  EXPECT_TRUE(doc.HasMember("status"));
  EXPECT_TRUE(doc.HasMember("uptimeEpochSec"));
  EXPECT_TRUE(doc.HasMember("gateway"));
  EXPECT_TRUE(doc.HasMember("services"));
}

TEST(ServiceGatewayHealthTest, StoppedGatewayStatusIsStopped) {
  // A gateway that was never start()ed reports "stopped", not "healthy".
  ServiceGateway gw(19011, "/tmp/rdws_gw_test_health2.sock");

  auto doc = gw.getHealth();

  EXPECT_STREQ(doc["status"].GetString(), "stopped");
}

TEST(ServiceGatewayHealthTest, GatewayObjectHasPortAndSocket) {
  ServiceGateway gw(19012, "/tmp/rdws_gw_test_health3.sock");

  auto doc = gw.getHealth();
  ASSERT_TRUE(doc["gateway"].IsObject());

  const auto& gobj = doc["gateway"];
  EXPECT_TRUE(gobj.HasMember("tcpPort"));
  EXPECT_TRUE(gobj.HasMember("unixSocket"));
  EXPECT_TRUE(gobj.HasMember("activeConnections"));
  EXPECT_TRUE(gobj.HasMember("pendingRequests"));

  EXPECT_EQ(gobj["tcpPort"].GetInt(), 19012);
  EXPECT_STREQ(gobj["unixSocket"].GetString(), "/tmp/rdws_gw_test_health3.sock");
}

TEST(ServiceGatewayHealthTest, FreshGatewayHasZeroConnectionsAndPending) {
  ServiceGateway gw(19013, "/tmp/rdws_gw_test_health4.sock");

  auto doc = gw.getHealth();
  const auto& gobj = doc["gateway"];

  EXPECT_EQ(gobj["activeConnections"].GetInt(), 0);
  EXPECT_EQ(gobj["pendingRequests"].GetInt(), 0);
}

TEST(ServiceGatewayHealthTest, FreshGatewayServicesArrayIsEmpty) {
  ServiceGateway gw(19014, "/tmp/rdws_gw_test_health5.sock");

  auto doc = gw.getHealth();
  ASSERT_TRUE(doc["services"].IsArray());
  EXPECT_TRUE(doc["services"].GetArray().Empty());
}

TEST(ServiceGatewayHealthTest, UptimeIsNonNegative) {
  ServiceGateway gw(19015, "/tmp/rdws_gw_test_health6.sock");

  auto doc = gw.getHealth();
  EXPECT_GE(doc["uptimeEpochSec"].GetInt64(), 0);
}
