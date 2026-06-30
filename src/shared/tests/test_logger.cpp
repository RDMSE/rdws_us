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
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    logPath = std::string("/tmp/rdws_logger_test_") + info->name() + "_" +
              std::to_string(::getpid()) + ".log";
    fs::remove(logPath);

    spdlog::drop_all();
    rdws::logger::init("test-logger", "info", "", logPath);
  }

  void TearDown() override {
    spdlog::drop_all();
    fs::remove(logPath);
  }

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
  spdlog::default_logger()->flush();
  EXPECT_TRUE(fs::exists(logPath));
}

TEST_F(LoggerTest, InfoEmitsCorrectFields) {
  rdws::logger::info("service_connected", "svc_001 (my_service) from /tmp/test.sock");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"info")"), std::string::npos);
  EXPECT_NE(content.find(R"("message":"service_connected")"), std::string::npos);
  EXPECT_NE(content.find("svc_001"), std::string::npos);
  EXPECT_NE(content.find(R"("ts":)"), std::string::npos);
}

TEST_F(LoggerTest, WarnEmitsCorrectFields) {
  rdws::logger::warn("service_disconnected", "svc_002 connection_reset");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"warn")"), std::string::npos);
  EXPECT_NE(content.find(R"("message":"service_disconnected")"), std::string::npos);
  EXPECT_NE(content.find("svc_002"), std::string::npos);
}

TEST_F(LoggerTest, ErrorEmitsCorrectFields) {
  rdws::logger::error("fatal error", "stack trace here");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"("event":"error")"), std::string::npos);
  EXPECT_NE(content.find(R"("message":"fatal error")"), std::string::npos);
  EXPECT_NE(content.find("stack trace here"), std::string::npos);
}

TEST_F(LoggerTest, MultipleEventsAreOnSeparateLines) {
  rdws::logger::info("http_request", "req_1 POST ping /invoke/ping");
  rdws::logger::info("http_response", "req_1 ping status=200 latency=10ms");

  const std::string content = readLog();
  const long lineCount = std::count(content.begin(), content.end(), '\n');
  EXPECT_EQ(lineCount, 2);
}

TEST_F(LoggerTest, JsonEscapesSpecialCharactersInValues) {
  rdws::logger::warn("service_disconnected", R"(svc_003 err "test" path\file)");

  const std::string content = readLog();
  EXPECT_NE(content.find(R"(err \"test\" path\\file)"), std::string::npos);
}

TEST_F(LoggerTest, WarnAndErrorDoNotCrash) {
  EXPECT_NO_THROW(rdws::logger::warn("something odd happened", "context_info"));
  EXPECT_NO_THROW(rdws::logger::error("fatal error", "stack trace here"));

  const std::string content = readLog();
  EXPECT_NE(content.find("something odd happened"), std::string::npos);
  EXPECT_NE(content.find("fatal error"), std::string::npos);
}
