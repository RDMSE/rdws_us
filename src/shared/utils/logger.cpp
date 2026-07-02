#include "logger.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace rdws::utils::logger {

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
std::string jsonEscape(std::string_view s) {
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
std::string buildJson(const std::string_view event,
                      const std::initializer_list<std::string> fields) {
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

/// Instance identifier embedded in every log line, set once by init().
std::string g_serviceId;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(
        const std::string_view name,
        const std::string_view level,
        const std::string_view serviceId,
        const std::string_view logFile) {
  g_serviceId = std::string(serviceId);

  std::vector<spdlog::sink_ptr> sinks;

  // Always log to stdout (with colour).
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

  // Rotating file sink — plain text (no ANSI codes). Path defaults to
  // "logs/<name>.log" so every process gets its own file without callers
  // having to wire a CLI argument; pass logFile to override.
  const std::string filePath =
      logFile.empty() ? "logs/" + std::string(name) + ".log" : std::string(logFile);
  if (const auto parent = std::filesystem::path(filePath).parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  constexpr auto maxBytes = static_cast<const std::size_t>(10 * 1024 * 1024); // 10 MB
  constexpr std::size_t maxFiles = 3;
  sinks.push_back(
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filePath, maxBytes, maxFiles));

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

void info(const std::string_view message, const std::string_view context) {
  spdlog::info(buildJson(
      "info", {kv("service_id", g_serviceId), kv("message", message), kv("context", context)}));
}

void warn(const std::string_view message, const std::string_view context) {
  spdlog::warn(buildJson(
      "warn", {kv("service_id", g_serviceId), kv("message", message), kv("context", context)}));
}

void error(const std::string_view message, const std::string_view context) {
  spdlog::error(buildJson(
      "error", {kv("service_id", g_serviceId), kv("message", message), kv("context", context)}));
}

} // namespace rdws::utils::logger
