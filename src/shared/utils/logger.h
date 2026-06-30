#pragma once

#include <string>
#include <string_view>

namespace rdws::logger {

/// Initialize the logger. Call once at startup.
/// @param name      Logger name, also used to derive the log file path
///                  ("logs/<name>.log"). Shown in logs (e.g. "rdws-gateway").
/// @param level     Minimum level: "trace","debug","info","warn","error","off".
/// @param serviceId Instance identifier (e.g. "device_config_001"), embedded
///                  as "service_id" in every log line. Empty = field omitted.
/// @param logFile   Overrides the derived log file path. Empty string = use
///                  "logs/<name>.log".
void init(std::string_view name = "rdws-service", std::string_view level = "info",
          std::string_view serviceId = "", std::string_view logFile = "");

// ---------------------------------------------------------------------------
// Structured log helpers — each emits one JSON line.
// ---------------------------------------------------------------------------

/// Generic informational message.
void info(std::string_view message, std::string_view context = "");

/// Generic warning.
void warn(std::string_view message, std::string_view context = "");

/// Generic error.
void error(std::string_view message, std::string_view context = "");

} // namespace rdws::logger
