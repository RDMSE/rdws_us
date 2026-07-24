#include "Services/ServiceClient.h"
#include "Services/ServiceGateway.h"
#include "../../shared/utils/json_helper.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

using namespace servicegateway;
namespace json = rdws::utils::json;

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

    const std::string msg = json::getString(req, "message").value_or("");
    const std::string result = "echo: " + msg;

    rapidjson::Document resp;
    resp.SetObject();
    auto& alloc = resp.GetAllocator();

    rapidjson::Value respValue = json::JsonObj(alloc)
        .set("result", result)
        .set("status", "success")
        .take();
    respValue.Swap(resp);
    return resp;
  });

  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  rapidjson::Document payload;
  payload.SetObject();
  auto& alloc = payload.GetAllocator();
  rapidjson::Value payloadValue = json::JsonObj(alloc)
      .set("capability", "echo")
      .set("message", "hello")
      .take();
  payloadValue.Swap(payload);

  const std::string requestId = gw->sendRequest("echo", payload);
  ASSERT_FALSE(requestId.empty()) << "sendRequest should return a requestId";

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(5000));

  EXPECT_EQ(result.state, RequestState::COMPLETED);
  EXPECT_EQ(result.statusCode, 200);
  EXPECT_FALSE(result.responsePayload.empty());

  rapidjson::Document resp;
  resp.Parse(result.responsePayload.c_str());
  ASSERT_FALSE(resp.HasParseError());
  const auto& responseValue = json::getString(resp, "result");
  ASSERT_TRUE(responseValue.has_value());
  EXPECT_EQ(responseValue.value(), "echo: hello");
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

// ---------------------------------------------------------------------------
// Scenario 6 — Service reconnects with the same serviceId → new request must
// reach the NEW connection, not the stale one.
//
// Regressão: ServiceGateway::closeConnection não removia a entrada de
// activeConnections nem fechava o fd no SO. Numa reconexão do mesmo serviço,
// sendDirectRequest (que itera o map em ordem ascendente de fd) quase sempre
// encontrava a entrada antiga e morta antes da nova, roteando a mensagem pro
// buraco — sem erro, sem timeout, silenciosamente perdida. Só aparecia após a
// PRIMEIRA reconexão (deploy, restart, crash), nunca na conexão inicial.
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, ServiceReconnects_SameId_NewRequestReachesNewConnection) {
  const std::string sock = "/tmp/rdws_gw_integ_6.sock";
  ASSERT_TRUE(startGateway(19205, sock)) << "Gateway failed to start";

  // First connection: register, then disconnect cleanly (no in-flight request).
  startClient(makeIdentity("integ_reconnect_001", {"reconnect"}), "unix://" + sock);
  client->setRequestHandler([](const rapidjson::Document&) -> rapidjson::Document {
    rapidjson::Document resp;
    resp.SetObject();
    return resp;
  });
  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  client->stop();
  if (clientThread.joinable()) {
    clientThread.join();
  }

  // Second connection, same serviceId — simulates a container/process restart.
  auto secondHandlerCalled = std::make_shared<std::atomic<bool>>(false);
  auto secondClient =
      std::make_unique<ServiceClient>(makeIdentity("integ_reconnect_001", {"reconnect"}),
                                      "unix://" + sock);
  secondClient->setRequestHandler(
      [secondHandlerCalled](const rapidjson::Document&) -> rapidjson::Document {
        secondHandlerCalled->store(true);
        rapidjson::Document resp;
        resp.SetObject();
        resp.AddMember("status", "success", resp.GetAllocator());
        return resp;
      });
  std::thread secondClientThread([&secondClient]() { secondClient->run(); });

  // Wait for the new connection to register (gw->getHealth() will show 1 service
  // again once re-registered — same count as before, so poll for a fresh response
  // instead of relying on waitForRegistration's "non-empty" check).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  bool reconnected = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto health = gw->getHealth();
    if (health.HasMember("services") && health["services"].IsArray() &&
        !health["services"].GetArray().Empty()) {
      reconnected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(reconnected) << "Service did not re-register in time";

  rapidjson::Document payload;
  payload.SetObject();

  const std::string requestId = gw->sendRequest("reconnect", payload);
  ASSERT_FALSE(requestId.empty()) << "sendRequest should find the reconnected service";

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(2000));

  secondClient->stop();
  secondClientThread.join();

  EXPECT_TRUE(secondHandlerCalled->load())
      << "Request was not delivered to the reconnected (new) connection — "
         "likely routed to a stale/dead connection left in activeConnections";
  EXPECT_EQ(result.state, RequestState::COMPLETED);
  EXPECT_EQ(result.statusCode, 200);
}

// ---------------------------------------------------------------------------
// Scenario 6 — ServiceClient::invoke(): one service synchronously calling another
// service's capability through the gateway (the client-originated INVOKE/
// INVOKE_RESPONSE protocol backing the device_service -> field_service
// convenience client described in docs/Plano_Gateway_HTTP.md).
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, Invoke_CallerReachesResponder_Success) {
  const std::string sock = "/tmp/rdws_gw_integ_6.sock";
  ASSERT_TRUE(startGateway(19206, sock)) << "Gateway failed to start";

  // "responder" service, plays the role of field_service.
  startClient(makeIdentity("integ_responder_001", {"invoke.target"}), "unix://" + sock);
  client->setRequestHandler([](const rapidjson::Document& req) -> rapidjson::Document {
    const std::string id = json::getString(req, "id").value_or("");
    rapidjson::Document resp;
    resp.SetObject();
    auto& alloc = resp.GetAllocator();
    rapidjson::Value respValue = json::JsonObj(alloc)
        .set("success", id == "42")
        .set("statusCode", id == "42" ? 200 : 404)
        .take();
    respValue.Swap(resp);
    return resp;
  });
  ASSERT_TRUE(waitForRegistration(*gw)) << "Responder did not register in time";

  // "caller" service, plays the role of device_service.
  ServiceClient caller(makeIdentity("integ_caller_001", {}), "unix://" + sock);
  std::thread callerThread([&caller]() { caller.run(); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!caller.isRegistered() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(caller.isRegistered()) << "Caller did not register in time";

  rapidjson::Document data;
  data.SetObject();
  auto& alloc = data.GetAllocator();
  rapidjson::Value dataValue = json::JsonObj(alloc).set("id", "42").take();
  dataValue.Swap(data);

  const InvokeResult result = caller.invoke("invoke.target", data, std::chrono::milliseconds(2000));

  caller.stop();
  callerThread.join();

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.statusCode, 200);
  ASSERT_FALSE(result.responsePayload.empty());

  rapidjson::Document resp;
  resp.Parse(result.responsePayload.c_str());
  ASSERT_FALSE(resp.HasParseError());
  EXPECT_TRUE(json::getBool(resp, "success").value_or(false));
}

// ---------------------------------------------------------------------------
// Scenario 6b — regression: a SINGLE ServiceClient instance must reconnect on
// its own after the broker connection drops (e.g. gateway restart), without
// the owning process re-creating the client. Covers the run()/messageLoop()
// reconnect-with-backoff loop added after SensorSimulatorService went silent
// for ~20min following a dropped connection with no automatic recovery.
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, ClientReconnectsAutomatically_AfterGatewayRestart) {
  const std::string sock = "/tmp/rdws_gw_integ_6b.sock";
  const int port = 19210;

  ASSERT_TRUE(startGateway(port, sock)) << "Gateway failed to start";

  startClient(makeIdentity("integ_autoreconnect_001", {"echo"}), "unix://" + sock);
  client->setRequestHandler([](const rapidjson::Document&) -> rapidjson::Document {
    rapidjson::Document resp;
    resp.SetObject();
    resp.AddMember("success", true, resp.GetAllocator());
    return resp;
  });
  ASSERT_TRUE(waitForRegistration(*gw)) << "Service did not register in time";

  // Kill the gateway out from under the still-running client — this closes the
  // client's socket fd server-side, so its messageLoop() sees recv() fail and
  // (with the fix) tears down and retries connect() on its own, without stop()
  // ever being called on the client.
  gw->stop();

  // Bring a brand-new gateway back up on the same addresses. The client thread
  // is still alive (never stopped) and must reconnect + re-register by itself.
  gw = std::make_unique<ServiceGateway>(port, sock);
  ASSERT_TRUE(gw->start()) << "Gateway failed to restart";
  ASSERT_TRUE(waitForSocket(sock)) << "Restarted gateway socket did not appear";

  ASSERT_TRUE(waitForRegistration(*gw, std::chrono::milliseconds(10000)))
      << "Client did not reconnect and re-register on its own after gateway restart";

  rapidjson::Document payload;
  payload.SetObject();

  const std::string requestId = gw->sendRequest("echo", payload);
  ASSERT_FALSE(requestId.empty()) << "sendRequest should find the auto-reconnected service";

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(2000));

  EXPECT_EQ(result.state, RequestState::COMPLETED);
  EXPECT_EQ(result.statusCode, 200);
}

// ---------------------------------------------------------------------------
// Scenario 7 — ServiceClient::invoke(): no service registered for the capability
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, Invoke_NoServiceForCapability_ReturnsFailure) {
  const std::string sock = "/tmp/rdws_gw_integ_7.sock";
  ASSERT_TRUE(startGateway(19207, sock)) << "Gateway failed to start";

  ServiceClient caller(makeIdentity("integ_caller_002", {}), "unix://" + sock);
  std::thread callerThread([&caller]() { caller.run(); });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (!caller.isRegistered() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(caller.isRegistered()) << "Caller did not register in time";

  rapidjson::Document data;
  data.SetObject();

  const InvokeResult result =
      caller.invoke("no.such.capability", data, std::chrono::milliseconds(1000));

  caller.stop();
  callerThread.join();

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.statusCode, 503);
}

// ---------------------------------------------------------------------------
// Scenario 7b — regression: invoke() must inject "capability" into the payload
// itself, not just the wire envelope. CapabilityRouter::dispatchCapability (used by
// every App*Service, e.g. field_service) reads "capability" from inside the request
// document — the same reason HttpGateway injects it manually for HTTP-triggered
// requests. Without this, the target responds 404 "Unknown capability: " (empty).
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, Invoke_InjectsCapabilityIntoPayload) {
  const std::string sock = "/tmp/rdws_gw_integ_7b.sock";
  ASSERT_TRUE(startGateway(19209, sock)) << "Gateway failed to start";

  startClient(makeIdentity("integ_responder_002", {"field.get"}), "unix://" + sock);
  client->setRequestHandler([](const rapidjson::Document& req) -> rapidjson::Document {
    // Mirrors rdws::utils::dispatchCapability: reads "capability" from the payload,
    // not the wire envelope.
    const std::string cap = json::getString(req, "capability").value_or("");
    rapidjson::Document resp;
    resp.SetObject();
    auto& alloc = resp.GetAllocator();
    rapidjson::Value respValue = json::JsonObj(alloc)
        .set("success", cap == "field.get")
        .set("statusCode", cap == "field.get" ? 200 : 404)
        .take();
    respValue.Swap(resp);
    return resp;
  });
  ASSERT_TRUE(waitForRegistration(*gw)) << "Responder did not register in time";

  ServiceClient caller(makeIdentity("integ_caller_003", {}), "unix://" + sock);
  std::thread callerThread([&caller]() { caller.run(); });
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (!caller.isRegistered() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(caller.isRegistered()) << "Caller did not register in time";
  }

  rapidjson::Document data;
  data.SetObject();
  auto& alloc = data.GetAllocator();
  rapidjson::Value pathParams = json::JsonObj(alloc).set("id", "42").take();
  rapidjson::Value dataValue =
      json::JsonObj(alloc).setValue("pathParameters", std::move(pathParams)).take();
  data.Swap(dataValue);

  const InvokeResult result = caller.invoke("field.get", data, std::chrono::milliseconds(2000));

  caller.stop();
  callerThread.join();

  ASSERT_TRUE(result.success) << "error=" << result.errorMessage;
  rapidjson::Document resp;
  resp.Parse(result.responsePayload.c_str());
  ASSERT_FALSE(resp.HasParseError());
  EXPECT_TRUE(json::getBool(resp, "success").value_or(false));
}

// ---------------------------------------------------------------------------
// Scenario 8 — regression: a service's own request handler calling invoke() while
// handling an inbound REQUEST must not deadlock. Before handleRequest() dispatched
// the handler onto its own thread, this scenario hung until invoke()'s timeout: the
// handler ran inline on the message loop thread, so that thread could never get
// back to recv() to read the INVOKE_RESPONSE it was itself blocking on (this is
// exactly the device_service -> field_service shape via FieldServiceClient).
// ---------------------------------------------------------------------------
TEST_F(GatewayIntegrationTest, HandlerCallingInvoke_DoesNotDeadlockOwnMessageLoop) {
  const std::string sock = "/tmp/rdws_gw_integ_8.sock";
  ASSERT_TRUE(startGateway(19208, sock)) << "Gateway failed to start";

  // "downstream" service — analogous to field_service.
  ServiceClient downstream(makeIdentity("integ_downstream_001", {"downstream.get"}),
                           "unix://" + sock);
  std::thread downstreamThread([&downstream]() { downstream.run(); });
  downstream.setRequestHandler([](const rapidjson::Document&) -> rapidjson::Document {
    rapidjson::Document resp;
    resp.SetObject();
    resp.AddMember("success", true, resp.GetAllocator());
    return resp;
  });

  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (!downstream.isRegistered() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(downstream.isRegistered()) << "Downstream service did not register in time";
  }

  // "upstream" service — analogous to device_service: its own request handler
  // calls invoke() on the downstream capability before answering.
  startClient(makeIdentity("integ_upstream_001", {"upstream.create"}), "unix://" + sock);
  client->setRequestHandler([this](const rapidjson::Document&) -> rapidjson::Document {
    rapidjson::Document data;
    data.SetObject();
    const InvokeResult inner = client->invoke("downstream.get", data, std::chrono::milliseconds(2000));

    rapidjson::Document resp;
    resp.SetObject();
    resp.AddMember("innerSuccess", inner.success, resp.GetAllocator());
    return resp;
  });
  {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (!client->isRegistered() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(client->isRegistered()) << "Upstream service did not register in time";
  }

  // Drive it the same way an HTTP caller would: gateway sendRequest + waitForResponse.
  rapidjson::Document payload;
  payload.SetObject();
  auto& alloc = payload.GetAllocator();
  rapidjson::Value payloadValue = json::JsonObj(alloc).set("capability", "upstream.create").take();
  payloadValue.Swap(payload);

  const std::string requestId = gw->sendRequest("upstream.create", payload);
  ASSERT_FALSE(requestId.empty());

  const PendingRequest result = gw->waitForResponse(requestId, std::chrono::milliseconds(4000));

  downstream.stop();
  downstreamThread.join();

  ASSERT_EQ(result.state, RequestState::COMPLETED)
      << "Outer request timed out — the inner invoke() likely deadlocked the "
         "upstream service's own message loop";

  rapidjson::Document resp;
  resp.Parse(result.responsePayload.c_str());
  ASSERT_FALSE(resp.HasParseError());
  EXPECT_TRUE(json::getBool(resp, "innerSuccess").value_or(false));
}
