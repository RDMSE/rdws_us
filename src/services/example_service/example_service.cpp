//
// Example Service - ServiceGateway Architecture v2.0
// Demonstrates service creation using ServiceClient library
//

#include "../../service_broker/Services/ServiceClient.h"
#include "../../shared/utils/json_helper.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>

using namespace servicegateway;

class ExampleService {
private:
  ServiceIdentity identity;
  std::unique_ptr<ServiceClient> client;
  std::string gatewayAddress;
  bool developmentMode = false;
  std::atomic<bool> running{false};

  static std::string resolveCommand(const rapidjson::Document& request) {
    const auto& commandValue = rdws::utils::getString(request, "command");
    if (commandValue.has_value()) {
      return commandValue.value();
    }

    const auto& capabilityValue = rdws::utils::getString(request, "capability");
    if (capabilityValue.has_value()) {
      return capabilityValue.value();
    }

    const auto& lambdaEvent = rdws::utils::getObject(request, "lambdaEvent");
    if (lambdaEvent != nullptr) {
      const auto& pathParameters = rdws::utils::getObject(*lambdaEvent, "pathParameters");
      if (pathParameters != nullptr) {
        const auto& capabilityValue = rdws::utils::getString(*pathParameters, "capability");
        if (capabilityValue.has_value()) {
          return capabilityValue.value();
        }
      }
    }

    return "";
  }

  static void addMockUserResponse(rapidjson::Document& response,
                                  rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value user(rapidjson::kObjectType);
    user.AddMember("id", "usr_mock_001", allocator);
    user.AddMember("name", "Ada Lovelace", allocator);
    user.AddMember("email", "ada.lovelace@example.local", allocator);
    user.AddMember("plan", "enterprise", allocator);
    user.AddMember("active", true, allocator);
    user.AddMember("loginCount", 42, allocator);
    user.AddMember("lastLogin", "2026-05-15T12:00:00Z", allocator);

    response.AddMember("result", user, allocator);
    response.AddMember("status", "success", allocator);
  }

  static void addMockOrdersResponse(rapidjson::Document& response,
                                    rapidjson::Document::AllocatorType& allocator) {
    rapidjson::Value orders(rapidjson::kArrayType);

    rapidjson::Value order1(rapidjson::kObjectType);
    order1.AddMember("orderId", "ord_mock_1001", allocator);
    order1.AddMember("status", "processing", allocator);
    order1.AddMember("amount", 199.90, allocator);
    order1.AddMember("currency", "BRL", allocator);

    rapidjson::Value order2(rapidjson::kObjectType);
    order2.AddMember("orderId", "ord_mock_1002", allocator);
    order2.AddMember("status", "delivered", allocator);
    order2.AddMember("amount", 49.50, allocator);
    order2.AddMember("currency", "BRL", allocator);

    rapidjson::Value order3(rapidjson::kObjectType);
    order3.AddMember("orderId", "ord_mock_1003", allocator);
    order3.AddMember("status", "pending_payment", allocator);
    order3.AddMember("amount", 10.00, allocator);
    order3.AddMember("currency", "BRL", allocator);

    orders.PushBack(order1, allocator);
    orders.PushBack(order2, allocator);
    orders.PushBack(order3, allocator);

    rapidjson::Value payload(rapidjson::kObjectType);
    payload.AddMember("total", static_cast<int>(orders.Size()), allocator);
    payload.AddMember("orders", orders, allocator);

    response.AddMember("result", payload, allocator);
    response.AddMember("status", "success", allocator);
  }

  static void addMathResponse(const rapidjson::Document& request, rapidjson::Document& response,
                              rapidjson::Document::AllocatorType& allocator) {
    const double a = rdws::utils::getDouble(request, "a").value_or(0.0);
        // (request.HasMember("a") && request["a"].IsNumber()) ? request["a"].GetDouble() : 0.0;
    const double b = rdws::utils::getDouble(request, "b").value_or(0.0);
        // (request.HasMember("b") && request["b"].IsNumber()) ? request["b"].GetDouble() : 0.0;
    const auto& operation = rdws::utils::getString(request, "operation").value_or("add");

    rapidjson::Value result(rapidjson::kObjectType);
    result.AddMember("a", a, allocator);
    result.AddMember("b", b, allocator);
    result.AddMember("operation", rapidjson::Value(operation.c_str(), allocator), allocator);

    if (operation == "subtract") {
      result.AddMember("value", a - b, allocator);
    } else if (operation == "multiply") {
      result.AddMember("value", a * b, allocator);
    } else if (operation == "divide") {
      if (b == 0.0) {
        response.AddMember("error", "Division by zero", allocator);
        response.AddMember("status", "error", allocator);
        return;
      }
      result.AddMember("value", a / b, allocator);
    } else {
      result.AddMember("value", a + b, allocator);
    }

    response.AddMember("result", result, allocator);
    response.AddMember("status", "success", allocator);
  }

public:
  ExampleService(const std::string& serviceId, const std::string& machineName,
                 const bool devMode = false, std::string broker = "unix:///tmp/rdws_gateway.sock")
      : gatewayAddress(std::move(broker)), developmentMode(devMode) {

    // Setup service identity
    identity.machineName = machineName;
    identity.serviceName = "example_service";
    identity.serviceId = serviceId;
    identity.version = "v2.0.0";
    identity.environment = devMode ? "dev" : "prod";
    identity.maxConcurrent = 10;
    identity.capabilities = {"ping", "echo", "status", "info", "math"};
  }

  bool initialize() {
    std::cout << "[" << identity.serviceId << "] Initializing service..." << '\n';

    client = std::make_unique<ServiceClient>(identity, gatewayAddress);

    client->setRequestHandler([this](const rapidjson::Document& request) -> rapidjson::Document {
      return this->processRequest(request);
    });

    if (developmentMode) {
      std::cout << "[" << identity.serviceId << "] Debug mode enabled" << '\n';
    }

    return true;
  }

  void run() {
    running.store(true);

    std::cout << "[" << identity.serviceId << "] Starting service..." << '\n';
    std::cout << "[" << identity.serviceId << "] Machine: " << identity.machineName << '\n';
    std::cout << "[" << identity.serviceId << "] Version: " << identity.version << '\n';
    std::cout << "[" << identity.serviceId << "] Capabilities: ";

    for (size_t i = 0; i < identity.capabilities.size(); ++i) {
      std::cout << identity.capabilities[i];
      if (i < identity.capabilities.size() - 1) {
        std::cout << ", ";
      }
    }
    std::cout << '\n';
    std::cout << "[" << identity.serviceId << "] Gateway: " << gatewayAddress << '\n';

    client->run();

    std::cout << "[" << identity.serviceId << "] Service stopped" << '\n';
  }

  void shutdown() {
    std::cout << "[" << identity.serviceId << "] Shutdown requested" << '\n';
    running.store(false);

    if (client) {
      client->stop();
    }
  }

private:
  [[nodiscard]] rapidjson::Document processRequest(const rapidjson::Document& request) const {
    const std::string command = resolveCommand(request);

    std::cout << "[" << identity.serviceId << "] Processing: " << command << '\n';

    rapidjson::Document response;
    response.SetObject();
    auto& allocator = response.GetAllocator();

    response.AddMember("serviceId", rapidjson::Value(identity.serviceId.c_str(), allocator),
                       allocator);
    response.AddMember("timestamp",
                       static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()),
                       allocator);

    if (command == "ping") {
      response.AddMember("result", "pong", allocator);
      response.AddMember("status", "success", allocator);
    } else if (command == "status") {
      response.AddMember("result", "running", allocator);
      response.AddMember("status", "success", allocator);
      response.AddMember("uptime", static_cast<int64_t>(identity.getUptime().count()), allocator);
      response.AddMember("load", static_cast<int64_t>(identity.currentLoad), allocator);
    } else if (command == "info") {
      rapidjson::Value resultObj(rapidjson::kObjectType);
      resultObj.AddMember("service", rapidjson::Value(identity.serviceName.c_str(), allocator),
                          allocator);
      resultObj.AddMember("id", rapidjson::Value(identity.serviceId.c_str(), allocator), allocator);
      resultObj.AddMember("machine", rapidjson::Value(identity.machineName.c_str(), allocator),
                          allocator);
      resultObj.AddMember("version", rapidjson::Value(identity.version.c_str(), allocator),
                          allocator);
      resultObj.AddMember("environment", rapidjson::Value(identity.environment.c_str(), allocator),
                          allocator);
      rapidjson::Value caps(rapidjson::kArrayType);
      for (const auto& cap : identity.capabilities) {
        caps.PushBack(rapidjson::Value(cap.c_str(), allocator), allocator);
      }
      resultObj.AddMember("capabilities", caps, allocator);
      response.AddMember("result", resultObj, allocator);
      response.AddMember("status", "success", allocator);
    } else if (command == "mock_user") {
      addMockUserResponse(response, allocator);
    } else if (command == "mock_orders") {
      addMockOrdersResponse(response, allocator);
    } else if (command == "math") {
      addMathResponse(request, response, allocator);
    } else if (command == "echo") {
      const auto& message = rdws::utils::getString(request, "message").value_or("");
      response.AddMember("result", rapidjson::Value(("echo: " + message).c_str(), allocator),
                         allocator);
      response.AddMember("status", "success", allocator);
    } else {
      response.AddMember(
          "error", rapidjson::Value(("Unknown command: " + command).c_str(), allocator), allocator);
      response.AddMember("status", "error", allocator);
      rapidjson::Value commands(rapidjson::kArrayType);
      commands.PushBack("ping", allocator);
      commands.PushBack("status", allocator);
      commands.PushBack("info", allocator);
      commands.PushBack("echo", allocator);
      commands.PushBack("math", allocator);
      commands.PushBack("mock_user", allocator);
      commands.PushBack("mock_orders", allocator);
      response.AddMember("availableCommands", commands, allocator);
    }

    // Simulate processing time
    std::this_thread::sleep_for(std::chrono::milliseconds(10 + (rand() % 50)));

    const auto& statusValue = rdws::utils::getString(response, "status");
    if (statusValue.has_value()) {
        std::cout << "[" << identity.serviceId << "] Response: " << statusValue.value() << '\n';
    }

    return response;
  }
};

// Global service instance for signal handling
ExampleService* globalService = nullptr;

void signalHandler(const int signal) {
  if ((globalService != nullptr) && (signal == SIGTERM || signal == SIGINT)) {
    globalService->shutdown();
  }
}

int main(const int argc, char* argv[]) {
  bool developmentMode = false;
  std::string gatewayAddress = "unix:///tmp/rdws_gateway.sock";
  std::string serviceId = "example_001";
  std::string machineName = "localhost";

  // Parse arguments
  if (argc >= 2 && std::string(argv[1]) == "--dev") {
    developmentMode = true;
    serviceId = "example_dev";
    machineName = "dev-machine";

    std::cout << "=== DEVELOPMENT MODE ===" << '\n';
    std::cout << "Connect to gateway for debugging" << '\n';
    std::cout << "========================" << '\n';
  } else if (argc >= 4) {
    serviceId = argv[1];
    machineName = argv[2];
    gatewayAddress = argv[3];
  } else if (argc != 1) {
    std::cerr << "Usage:" << '\n';
    std::cerr << "  Development: " << argv[0] << " --dev" << '\n';
    std::cerr << "  Production:  " << argv[0] << " <serviceId> <machineName> <gatewayAddress>"
              << '\n';
    std::cerr << "  Default:     " << argv[0] << '\n';
    return 1;
  }

  std::cout << "[" << serviceId << "] Example Service v2.0 (ServiceGateway Architecture)" << '\n';

  ExampleService service(serviceId, machineName, developmentMode, gatewayAddress);
  globalService = &service;

  signal(SIGTERM, signalHandler);
  signal(SIGINT, signalHandler);

  if (!service.initialize()) {
    std::cerr << "[" << serviceId << "] Failed to initialize service" << '\n';
    return 1;
  }

  service.run();

  std::cout << "[" << serviceId << "] Service exited gracefully" << '\n';
  return 0;
}
