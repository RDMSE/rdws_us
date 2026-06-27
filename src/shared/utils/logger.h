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

/// HTTP request received by the gateway.
void httpRequest(
                    std::string_view requestId,
                    std::string_view capability, std::string_view method,
                    std::string_view path
);

/// HTTP response sent to the caller.
void httpResponse(
                    std::string_view requestId,
                    std::string_view capability,
                    int statusCode,
                    long latencyMs
);

/// Request dispatched to a backend service.
void requestDispatched(
                    std::string_view requestId,
                    std::string_view capability,
                    std::string_view serviceId
);

/// Response correlated back from a service.
void responseCorrelated(
                    std::string_view requestId,
                    std::string_view serviceId,
                    std::string_view state
);

/// A service connected and registered.
void serviceConnected(
                    std::string_view serviceId,
                    std::string_view serviceName,
                    std::string_view address
);

/// A service disconnected.
void serviceDisconnected(
                    std::string_view serviceId,
                    std::string_view reason = ""
);

/// Generic warning.
void warn(std::string_view message, std::string_view context = "");

/// Generic error.
void error(std::string_view message, std::string_view context = "");

} // namespace rdws::logger
