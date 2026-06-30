#pragma once

#include <string>
#include <string_view>

namespace rdws::logger {

/// Initialize the logger. Call once at startup.
/// @param name    Logger name shown in logs (e.g. "rdws-gateway").
/// @param level   Minimum level: "trace","debug","info","warn","error","off".
/// @param logFile Path to rotating log file. Empty string = stdout only.
void init(std::string_view name = "rdws-gateway", std::string_view level = "info",
          std::string_view logFile = "");

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
