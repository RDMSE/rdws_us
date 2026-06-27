#include "logger.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace rdws::logger {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Return current UTC timestamp as ISO-8601 string with milliseconds.
std::string utcNow() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);

  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);

  std::ostringstream oss;
  oss << buf << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
  return oss.str();
}

/// Escape a string for safe embedding in JSON.
static std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

/// Build a one-liner JSON object from key-value pairs already serialized.
/// @param event  Event type label (no quotes needed — caller passes plain string).
/// @param fields  Pre-formatted "\"key\":value" fragments.
static std::string buildJson(std::string_view event, std::initializer_list<std::string> fields) {
  std::ostringstream oss;
  oss << R"({"ts":")" << utcNow() << R"(","event":")" << event << '"';
  for (const auto& f : fields) {
    oss << ',' << f;
  }
  oss << '}';
  return oss.str();
}

std::string kv(std::string_view key, std::string_view value) {
  return '"' + std::string(key) + "\":\"" + jsonEscape(value) + '"';
}

std::string kvInt(std::string_view key, long value) {
  return '"' + std::string(key) + "\":" + std::to_string(value);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(
        const std::string_view name, 
        const std::string_view level,
        const std::string_view logFile) {
  std::vector<spdlog::sink_ptr> sinks;

  // Always log to stdout (with colour).
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

  // Optional rotating file sink — plain text (no ANSI codes).
  if (!logFile.empty()) {
    // Create parent directory if it doesn't exist.
    if (const auto parent = std::filesystem::path(logFile).parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    constexpr std::size_t maxBytes = static_cast<const std::size_t>(10 * 1024 * 1024); // 10 MB
    constexpr std::size_t maxFiles = 3;
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(std::string(logFile),
                                                                           maxBytes, maxFiles));
  }

  const auto logger =
      std::make_shared<spdlog::logger>(std::string(name), sinks.begin(), sinks.end());
  spdlog::set_default_logger(logger);

  // Emit only the raw message — the JSON body carries its own timestamp.
  spdlog::set_pattern("%v");

  const auto lvl = spdlog::level::from_str(std::string(level));
  spdlog::set_level(lvl);

  // Flush immediately on every info/warn/error so the file is never stale.
  logger->flush_on(spdlog::level::trace);
}

void httpRequest(const std::string_view requestId, const std::string_view capability,
                 const std::string_view method, const std::string_view path) {
  spdlog::info(buildJson("http_request", {kv("requestId", requestId), kv("capability", capability),
                                          kv("method", method), kv("path", path)}));
}

void httpResponse(const std::string_view requestId, const std::string_view capability,
                  const int statusCode, const long latencyMs) {
  spdlog::info(
      buildJson("http_response", {kv("requestId", requestId), kv("capability", capability),
                                  kvInt("statusCode", statusCode), kvInt("latencyMs", latencyMs)}));
}

void requestDispatched(const std::string_view requestId, const std::string_view capability,
                       const std::string_view serviceId) {
  spdlog::info(
      buildJson("request_dispatched", {kv("requestId", requestId), kv("capability", capability),
                                       kv("serviceId", serviceId)}));
}

void responseCorrelated(const std::string_view requestId, const std::string_view serviceId,
                        const std::string_view state) {
  spdlog::info(buildJson("response_correlated", {kv("requestId", requestId),
                                                 kv("serviceId", serviceId), kv("state", state)}));
}

void serviceConnected(const std::string_view serviceId, const std::string_view serviceName,
                      const std::string_view address) {
  spdlog::info(
      buildJson("service_connected", {kv("serviceId", serviceId), kv("serviceName", serviceName),
                                      kv("address", address)}));
}

void serviceDisconnected(const std::string_view serviceId, const std::string_view reason) {
  spdlog::info(
      buildJson("service_disconnected", {kv("serviceId", serviceId), kv("reason", reason)}));
}

void warn(const std::string_view message, const std::string_view context) {
  spdlog::warn(buildJson("warn", {kv("message", message), kv("context", context)}));
}

void error(const std::string_view message, const std::string_view context) {
  spdlog::error(buildJson("error", {kv("message", message), kv("context", context)}));
}

} // namespace rdws::logger
