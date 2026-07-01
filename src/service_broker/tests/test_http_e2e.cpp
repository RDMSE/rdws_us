#include "Auth/AuthMiddleware.h"
#include "Config/GatewayConfig.h"
#include "HttpGateway.h"
#include "Services/ServiceClient.h"
#include "Services/ServiceGateway.h"
#include "../../shared/utils/json_helper.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <httplib.h>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <thread>

using namespace servicegateway;
namespace json = rdws::utils::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool waitForSocket(const std::string& sockPath,
                          std::chrono::milliseconds maxWait = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + maxWait;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(sockPath)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

static bool waitForHttp(int port,
                        std::chrono::milliseconds maxWait = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + maxWait;
  while (std::chrono::steady_clock::now() < deadline) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(0, 100'000);
    if (auto res = cli.Get("/health"); res) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

static bool
waitForRegistration(ServiceGateway& gw,
                    std::chrono::milliseconds maxWait = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + maxWait;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto health = gw.getHealth();
    if (health.HasMember("services") && health["services"].IsArray() &&
        !health["services"].GetArray().Empty()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

static ServiceIdentity makeIdentity(const std::string& serviceId,
                                    const std::vector<std::string>& caps) {
  ServiceIdentity id;
  id.serviceId = serviceId;
  id.serviceName = "e2e_test_service";
  id.machineName = "localhost";
  id.version = "v1.0.0";
  id.capabilities = caps;
  id.maxConcurrent = 10;
  return id;
}

static rapidjson::Document parseBody(const std::string& body) {
  rapidjson::Document doc;
  doc.Parse(body.c_str());
  return doc;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

// TCP broker ports: 18300–18309
// HTTP gateway ports: 19300–19309
// Socket paths:      /tmp/rdws_e2e_N.sock

class HttpE2ETest : public ::testing::Test {
protected:
  std::unique_ptr<ServiceGateway> gw;
  std::unique_ptr<HttpGateway> http;
  std::unique_ptr<ServiceClient> svc;
  std::thread svcThread;
  int httpPort = 0;

  void TearDown() override {
    if (svc) {
      svc->stop();
    }
    if (svcThread.joinable()) {
      svcThread.join();
    }
    if (http) {
      http->stop();
    }
    if (gw) {
      gw->stop();
    }
  }

  bool startStack(int port, const std::string& sock, AuthConfig authCfg = {}) {
    httpPort = port;
    gw = std::make_unique<ServiceGateway>(port + 1000, sock);
    if (!gw->start()) {
      return false;
    }
    if (!waitForSocket(sock)) {
      return false;
    }
    http = std::make_unique<HttpGateway>(*gw, port, "127.0.0.1", std::move(authCfg));
    if (!http->start()) {
      return false;
    }
    return waitForHttp(port);
  }

  void startMockService(const ServiceIdentity& id, const std::string& addr,
                        RequestHandler handler) {
    svc = std::make_unique<ServiceClient>(id, addr);
    svc->setRequestHandler(std::move(handler));
    svcThread = std::thread([this]() { svc->run(); });
  }

  httplib::Result httpGet(const std::string& path) {
    httplib::Client cli("127.0.0.1", httpPort);
    return cli.Get(path.c_str());
  }

  httplib::Result httpPost(const std::string& path, const std::string& body) {
    httplib::Client cli("127.0.0.1", httpPort);
    return cli.Post(path.c_str(), body, "application/json");
  }

  httplib::Result httpPostWithHeader(const std::string& path, const std::string& body,
                                     const httplib::Headers& headers) {
    httplib::Client cli("127.0.0.1", httpPort);
    return cli.Post(path.c_str(), headers, body, "application/json");
  }
};

// ── Scenario 1: Observability endpoints return 200 ────────────────────────────

TEST_F(HttpE2ETest, ObservabilityEndpoints_ReturnOk) {
  ASSERT_TRUE(startStack(19300, "/tmp/rdws_e2e_1.sock"));

  for (const auto& path : {"/status", "/health", "/metrics", "/connections"}) {
    auto res = httpGet(path);
    ASSERT_TRUE(res) << "No response from " << path;
    EXPECT_EQ(res->status, 200) << path << " returned " << res->status;

    rapidjson::Document doc = parseBody(res->body);
    EXPECT_FALSE(doc.HasParseError()) << path << " body is not valid JSON";
  }
}

// ── Scenario 2: Invoke with no backend service → 503 ─────────────────────────

TEST_F(HttpE2ETest, InvokeNoService_Returns503) {
  ASSERT_TRUE(startStack(19301, "/tmp/rdws_e2e_2.sock"));

  auto res = httpPost("/invoke/unknown-cap", R"({"message":"hello"})");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 503);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  const auto& errorValue = json::getString(doc, "error");
  ASSERT_TRUE(errorValue.has_value());
  EXPECT_NE(errorValue.value().find("unknown-cap"), std::string::npos);
}

// ── Scenario 3: Echo service responds → 200 with payload ──────────────────────

TEST_F(HttpE2ETest, EchoService_Returns200WithPayload) {
  ASSERT_TRUE(startStack(19302, "/tmp/rdws_e2e_3.sock"));

  startMockService(makeIdentity("e2e_echo", {"echo-e2e"}), "unix:///tmp/rdws_e2e_3.sock",
                   [](const rapidjson::Document& req) -> rapidjson::Document {
                     rapidjson::Document resp;
                     resp.SetObject();
                     auto& alloc = resp.GetAllocator();
                     const auto& msg = json::getString(req, "message").value_or("");
                     resp.AddMember("result", rapidjson::Value(("echo: " + msg).c_str(), alloc),
                                    alloc);
                     resp.AddMember("status", "success", alloc);
                     return resp;
                   });

  ASSERT_TRUE(waitForRegistration(*gw));

  auto res = httpPost("/invoke/echo-e2e", R"({"message":"world"})");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  const auto& resultValue = json::getString(doc, "result");
  ASSERT_TRUE(resultValue.has_value());
  EXPECT_STREQ(resultValue.value().c_str(), "echo: world");
}

// ── Scenario 4: Slow service exceeds capability timeout → 504 ─────────────────

TEST_F(HttpE2ETest, SlowService_Timeout_Returns504) {
  ASSERT_TRUE(startStack(19303, "/tmp/rdws_e2e_4.sock"));

  // Set a short timeout for this capability so the test finishes quickly.
  CapabilityConfig cfg;
  cfg.timeoutMs = std::chrono::milliseconds(400);
  gw->getConfig().setCapabilityConfig("slow-e2e", cfg);

  auto stopFlag = std::make_shared<std::atomic<bool>>(false);
  startMockService(makeIdentity("e2e_slow", {"slow-e2e"}), "unix:///tmp/rdws_e2e_4.sock",
                   [stopFlag](const rapidjson::Document&) -> rapidjson::Document {
                     for (int i = 0; i < 200 && !stopFlag->load(); ++i) {
                       std::this_thread::sleep_for(std::chrono::milliseconds(50));
                     }
                     rapidjson::Document resp;
                     resp.SetObject();
                     return resp;
                   });

  ASSERT_TRUE(waitForRegistration(*gw));

  auto res = httpPost("/invoke/slow-e2e", "{}");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 504);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  const auto& errorValue = json::getString(doc, "error");
  ASSERT_TRUE(errorValue.has_value());
  EXPECT_NE(errorValue.value().find("timed out"), std::string::npos);

  *stopFlag = true;
}

// ── Scenario 5: Auth required, missing key → 401 ──────────────────────────────

TEST_F(HttpE2ETest, Auth_MissingApiKey_Returns401) {
  AuthConfig authCfg;
  authCfg.mode = AuthMode::API_KEY;
  authCfg.apiKeys = {{"secret-key-123", "test-service"}};

  ASSERT_TRUE(startStack(19304, "/tmp/rdws_e2e_5.sock", std::move(authCfg)));

  // No auth header — expect 401.
  auto res = httpPost("/invoke/any-cap", "{}");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  EXPECT_TRUE(doc.HasMember("error"));
}

// ── Scenario 6: Auth required, valid API key → request reaches service ─────────

TEST_F(HttpE2ETest, Auth_ValidApiKey_RequestProceeds) {
  AuthConfig authCfg;
  authCfg.mode = AuthMode::API_KEY;
  authCfg.apiKeys = {{"secret-key-abc", "trusted-client"}};

  ASSERT_TRUE(startStack(19305, "/tmp/rdws_e2e_6.sock", std::move(authCfg)));

  startMockService(makeIdentity("e2e_auth_echo", {"auth-echo"}), "unix:///tmp/rdws_e2e_6.sock",
                   [](const rapidjson::Document&) -> rapidjson::Document {
                     rapidjson::Document resp;
                     resp.SetObject();
                     resp.AddMember("ok", true, resp.GetAllocator());
                     return resp;
                   });

  ASSERT_TRUE(waitForRegistration(*gw));

  httplib::Headers headers = {{"X-API-Key", "secret-key-abc"}};
  auto res = httpPostWithHeader("/invoke/auth-echo", "{}", headers);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  const auto& okValue = json::getBool(doc, "ok");
  ASSERT_TRUE(okValue.has_value());
  EXPECT_TRUE(okValue.value());
}

// ── Scenario 7: GET /requests/{id} for unknown id → 404 ───────────────────────

TEST_F(HttpE2ETest, RequestsEndpoint_UnknownId_Returns404) {
  ASSERT_TRUE(startStack(19306, "/tmp/rdws_e2e_7.sock"));

  auto res = httpGet("/requests/nonexistent-request-id-xyz");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── Scenario 8: EventRouter redirects capability to registered service ─────────

TEST_F(HttpE2ETest, EventRouter_RedirectsCapability_ServiceResponds) {
  ASSERT_TRUE(startStack(19307, "/tmp/rdws_e2e_8.sock"));

  // Service handles "target-cap", not "source-cap".
  startMockService(makeIdentity("e2e_router_svc", {"target-cap"}), "unix:///tmp/rdws_e2e_8.sock",
                   [](const rapidjson::Document&) -> rapidjson::Document {
                     rapidjson::Document resp;
                     resp.SetObject();
                     resp.AddMember("routed", true, resp.GetAllocator());
                     return resp;
                   });

  ASSERT_TRUE(waitForRegistration(*gw));

  // Add a routing rule: source-cap → target-cap.
  RoutingRule rule;
  rule.inputCapability = "source-cap";
  rule.outputCapability = "target-cap";
  rule.name = "e2e-redirect";
  gw->getEventRouter().addRule(std::move(rule));

  // POST to the source capability — should be transparently routed.
  auto res = httpPost("/invoke/source-cap", "{}");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);

  rapidjson::Document doc = parseBody(res->body);
  ASSERT_FALSE(doc.HasParseError());
  const auto& routedValue = json::getBool(doc, "routed");
  ASSERT_TRUE(routedValue.has_value());
  EXPECT_TRUE(routedValue.value());
}
