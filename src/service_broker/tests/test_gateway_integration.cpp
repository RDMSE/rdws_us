#include "Services/ServiceClient.h"
#include "Services/ServiceGateway.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

using namespace servicegateway;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ServiceIdentity makeIdentity(const std::string& serviceId,
                                    const std::vector<std::string>& caps) {
  ServiceIdentity id;
  id.serviceId = serviceId;
  id.serviceName = "integ_test_service";
  id.machineName = "localhost";
  id.version = "v1.0.0";
  id.capabilities = caps;
  id.maxConcurrent = 10;
  return id;
}

// Spin until the Unix socket file exists (gateway listener is ready).
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

// Spin until the gateway has at least one service registered (or we time out).
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

// ---------------------------------------------------------------------------
// Fixture for tests that need a gateway + client (ensures cleanup on failure)
// ---------------------------------------------------------------------------
class GatewayIntegrationTest : public ::testing::Test {
protected:
  std::unique_ptr<ServiceGateway> gw;
  std::unique_ptr<ServiceClient> client;
  std::thread clientThread;

  void TearDown() override {
    if (client) {
      client->stop();
    }
    if (clientThread.joinable()) {
      clientThread.join();
    }
    if (gw) {
      gw->stop();
    }
  }

  bool startGateway(int port, const std::string& sock) {
    gw = std::make_unique<ServiceGateway>(port, sock);
    if (!gw->start()) {
      return false;
    }
    return waitForSocket(sock);
  }

  void startClient(const ServiceIdentity& id, const std::string& addr) {
    client = std::make_unique<ServiceClient>(id, addr);
    clientThread = std::thread([this]() { client->run(); });
  }
};

// ---------------------------------------------------------------------------
// Scenario 1 — No service registered: sendRequest returns empty string (→503)
// ---------------------------------------------------------------------------
TEST(GatewayStandaloneTest, NoService_SendRequest_ReturnsEmpty) {
  // Gateway not started — no services in registry.
  ServiceGateway gw(19200, "/tmp/rdws_gw_integ_1.sock");

  rapidjson::Document payload;
  payload.SetObject();

  const std::string requestId = gw.sendRequest("ping", payload);
  EXPECT_TRUE(requestId.empty());
}

// ---------------------------------------------------------------------------
// Scenario 2 — waitForResponse with unknown requestId returns FAILED/404
//              immediately (no timeout needed).
// ---------------------------------------------------------------------------
TEST(GatewayStandaloneTest, WaitForUnknownRequest_ReturnsFailed404) {
  ServiceGateway gw(19201, "/tmp/rdws_gw_integ_2.sock");

  const PendingRequest result =
      gw.waitForResponse("nonexistent-request-id", std::chrono::milliseconds(5000));

  EXPECT_EQ(result.state, RequestState::FAILED);
  EXPECT_EQ(result.statusCode, 404);
}

// ---------------------------------------------------------------------------
// Scenario 3 — Full success cycle: gateway + ServiceClient + echo request
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, EchoRequest_ServiceResponds_Success) {
  const std::string sock = "/tmp/rdws_gw_integ_3.sock";
  ASSERT_TRUE(startGateway(19202, sock)) << "Gateway failed to start";

  startClient(makeIdentity("integ_echo_001", {"echo"}), "unix://" + sock);
  client->setRequestHandler([](const rapidjson::Document& req) -> rapidjson::Document {
    rapidjson::Document resp;
    resp.SetObject();
    auto& alloc = resp.GetAllocator();
    const std::string msg =
        (req.HasMember("message") && req["message"].IsString()) ? req["message"].GetString() : "";
    const std::string result = "echo: " + msg;
    resp.AddMember("result", rapidjson::Value(result.c_str(), alloc), alloc);
    resp.AddMember("status", "success", alloc);
    return resp;
  });

  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  rapidjson::Document payload;
  payload.SetObject();
  auto& alloc = payload.GetAllocator();
  payload.AddMember("capability", "echo", alloc);
  payload.AddMember("message", "hello", alloc);

  const std::string requestId = gw->sendRequest("echo", payload);
  ASSERT_FALSE(requestId.empty()) << "sendRequest should return a requestId";

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(5000));

  EXPECT_EQ(result.state, RequestState::COMPLETED);
  EXPECT_EQ(result.statusCode, 200);
  EXPECT_FALSE(result.responsePayload.empty());

  rapidjson::Document resp;
  resp.Parse(result.responsePayload.c_str());
  ASSERT_FALSE(resp.HasParseError());
  ASSERT_TRUE(resp.HasMember("result") && resp["result"].IsString());
  EXPECT_STREQ(resp["result"].GetString(), "echo: hello");
}

// ---------------------------------------------------------------------------
// Scenario 4 — Slow service: waitForResponse times out → TIMED_OUT / 504
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, SlowService_WaitForResponse_TimesOut) {
  const std::string sock = "/tmp/rdws_gw_integ_4.sock";
  ASSERT_TRUE(startGateway(19203, sock)) << "Gateway failed to start";

  // Use shared_ptr so the flag outlives the test body (TearDown must join the thread).
  auto stopHandler = std::make_shared<std::atomic<bool>>(false);

  startClient(makeIdentity("integ_slow_001", {"slow"}), "unix://" + sock);
  client->setRequestHandler([stopHandler](const rapidjson::Document&) -> rapidjson::Document {
    for (int i = 0; i < 100 && !stopHandler->load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    rapidjson::Document resp;
    resp.SetObject();
    return resp;
  });

  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  rapidjson::Document payload;
  payload.SetObject();

  const std::string requestId = gw->sendRequest("slow", payload);
  ASSERT_FALSE(requestId.empty());

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(300));

  EXPECT_EQ(result.state, RequestState::TIMED_OUT);
  EXPECT_EQ(result.statusCode, 504);

  *stopHandler = true; // signal handler to exit so TearDown can join
}

// ---------------------------------------------------------------------------
// Scenario 5 — Service disconnects mid-flight → FAILED / 503
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, ServiceDisconnects_InFlight_RequestFails) {
  const std::string sock = "/tmp/rdws_gw_integ_5.sock";
  ASSERT_TRUE(startGateway(19204, sock)) << "Gateway failed to start";

  // Use shared_ptr so flags outlive the test body.
  auto handlerStarted = std::make_shared<std::atomic<bool>>(false);
  auto stopHandler = std::make_shared<std::atomic<bool>>(false);

  startClient(makeIdentity("integ_disc_001", {"disconnect"}), "unix://" + sock);
  client->setRequestHandler(
      [handlerStarted, stopHandler](const rapidjson::Document&) -> rapidjson::Document {
        handlerStarted->store(true);
        for (int i = 0; i < 200 && !stopHandler->load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        rapidjson::Document resp;
        resp.SetObject();
        return resp;
      });

  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  rapidjson::Document payload;
  payload.SetObject();

  const std::string requestId = gw->sendRequest("disconnect", payload);
  ASSERT_FALSE(requestId.empty());

  // Wait until the handler has started processing the request.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (!handlerStarted->load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(handlerStarted->load()) << "Handler did not start in time";

  // Disconnect the service while the request is in-flight.
  *stopHandler = true;
  client->stop();
  if (clientThread.joinable()) {
    clientThread.join();
  }

  // Gateway should fail the pending request immediately upon disconnect.
  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(2000));

  EXPECT_EQ(result.state, RequestState::FAILED);
  EXPECT_EQ(result.statusCode, 503);
}
