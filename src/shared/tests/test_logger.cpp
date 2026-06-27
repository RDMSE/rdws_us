#include "utils/logger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture: isolates spdlog global state and temp files across test cases.
// ---------------------------------------------------------------------------
class LoggerTest : public ::testing::Test {
protected:
  std::string logPath;

  void SetUp() override {
    // Use a unique path per test to avoid cross-test pollution.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    logPath = std::string("/tmp/rdws_logger_test_") + info->name() + "_" +
              std::to_string(::getpid()) + ".log";
    fs::remove(logPath);

    // Drop any logger left by a previous test (spdlog global state).
    spdlog::drop_all();
    rdws::logger::init("test-logger", "info", logPath);
  }

  void TearDown() override {
    spdlog::drop_all(); // release file handles before deleting
    fs::remove(logPath);
  }

  // Read the entire log file and flush first.
  std::string readLog() {
    spdlog::default_logger()->flush();
    std::ifstream f(logPath);
    return std::string(std::istreambuf_iterator<char>(f), {});
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(LoggerTest, InitCreatesLogFile) {
  // File should exist after init even before any log calls.
  spdlog::default_logger()->flush();
  EXPECT_TRUE(fs::exists(logPath));
}

TEST_F(LoggerTest, ServiceConnectedEmitsCorrectFields) {
  rdws::logger::serviceConnected("svc_001", "my_service", "/tmp/test.sock");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"service_connected")"), std::string::npos);
  EXPECT_NE(content.find(R"("serviceId":"svc_001")"), std::string::npos);
  EXPECT_NE(content.find(R"("serviceName":"my_service")"), std::string::npos);
  EXPECT_NE(content.find(R"("address":"/tmp/test.sock")"), std::string::npos);
  EXPECT_NE(content.find(R"("ts":)"), std::string::npos);
}

TEST_F(LoggerTest, ServiceDisconnectedEmitsCorrectFields) {
  rdws::logger::serviceDisconnected("svc_002", "connection_reset");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"service_disconnected")"), std::string::npos);
  EXPECT_NE(content.find(R"("serviceId":"svc_002")"), std::string::npos);
  EXPECT_NE(content.find(R"("reason":"connection_reset")"), std::string::npos);
}

TEST_F(LoggerTest, HttpRequestEmitsCorrectFields) {
  rdws::logger::httpRequest("req_123", "ping", "POST", "/invoke/ping");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"http_request")"), std::string::npos);
  EXPECT_NE(content.find(R"("requestId":"req_123")"), std::string::npos);
  EXPECT_NE(content.find(R"("capability":"ping")"), std::string::npos);
  EXPECT_NE(content.find(R"("method":"POST")"), std::string::npos);
  EXPECT_NE(content.find(R"("path":"/invoke/ping")"), std::string::npos);
}

TEST_F(LoggerTest, HttpResponseEmitsCorrectFields) {
  rdws::logger::httpResponse("req_456", "echo", 200, 42);

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"http_response")"), std::string::npos);
  EXPECT_NE(content.find(R"("requestId":"req_456")"), std::string::npos);
  EXPECT_NE(content.find(R"("capability":"echo")"), std::string::npos);
  EXPECT_NE(content.find(R"("statusCode":200)"), std::string::npos);
  EXPECT_NE(content.find(R"("latencyMs":42)"), std::string::npos);
}

TEST_F(LoggerTest, ResponseCorrelatedEmitsCorrectFields) {
  rdws::logger::responseCorrelated("req_789", "svc_001", "completed");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"response_correlated")"), std::string::npos);
  EXPECT_NE(content.find(R"("requestId":"req_789")"), std::string::npos);
  EXPECT_NE(content.find(R"("serviceId":"svc_001")"), std::string::npos);
  EXPECT_NE(content.find(R"("state":"completed")"), std::string::npos);
}

TEST_F(LoggerTest, MultipleEventsAreOnSeparateLines) {
  rdws::logger::httpRequest("req_1", "ping", "POST", "/invoke/ping");
  rdws::logger::httpResponse("req_1", "ping", 200, 10);

  const std::string content = readLog();
  // Two JSON objects, each on its own line.
  const long lineCount = std::count(content.begin(), content.end(), '\n');
  EXPECT_EQ(lineCount, 2);
}

TEST_F(LoggerTest, JsonEscapesSpecialCharactersInValues) {
  // A reason containing a double quote and backslash.
  rdws::logger::serviceDisconnected("svc_003", R"(err "test" path\file)");

  const std::string content = readLog();
  // The log file must not have unescaped quotes breaking the JSON.
  EXPECT_NE(content.find(R"(err \"test\" path\\file)"), std::string::npos);
}

TEST_F(LoggerTest, WarnAndErrorDoNotCrash) {
  EXPECT_NO_THROW(rdws::logger::warn("something odd happened", "context_info"));
  EXPECT_NO_THROW(rdws::logger::error("fatal error", "stack trace here"));

  const std::string content = readLog();
  EXPECT_NE(content.find("something odd happened"), std::string::npos);
  EXPECT_NE(content.find("fatal error"), std::string::npos);
}
