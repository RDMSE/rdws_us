#pragma once

#include <cstdint>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace servicegateway {

// ─── Configuration ───────────────────────────────────────────────────────────

enum class AuthMode : uint8_t {
  NONE,    ///< No authentication required (default).
  API_KEY, ///< Static API key via X-API-Key header or Authorization: Bearer.
  JWT      ///< JWT Bearer token — HS256 HMAC signature verification.
};

struct AuthConfig {
  AuthMode mode = AuthMode::NONE;

  // ── API_KEY mode ────────────────────────────────────────────────────────
  /// Map of raw key → display name / identity label.
  /// Example: {"sk_abc123" → "my-service", "sk_xyz456" → "admin"}.
  std::unordered_map<std::string, std::string> apiKeys;

  // ── JWT mode ────────────────────────────────────────────────────────────
  std::string jwtSecret;   ///< HMAC-SHA256 signing secret.
  std::string jwtIssuer;   ///< Expected "iss" claim — empty = skip check.
  std::string jwtAudience; ///< Expected "aud" claim — empty = skip check.

  // ── Path policy ─────────────────────────────────────────────────────────
  /// Path prefixes that bypass auth even when a mode is active.
  std::vector<std::string> publicPaths = {
      "/health", "/status",   "/metrics",           "/connections", "/events",
      "/routes", "/requests", "/invoke/auth.login", "/auth/login"};
};

// ─── Framework-agnostic request view ─────────────────────────────────────────

/// Minimal view of an incoming HTTP request, decoupled from any HTTP framework.
/// Build one from an httplib::Request in HttpGateway before calling authenticate().
struct AuthHttpRequest {
  std::string path;
  /// Header map (case-sensitive key as received).  Typically lower-case
  /// after normalisation, but callers should ensure consistent casing.
  std::unordered_multimap<std::string, std::string> headers;

  [[nodiscard]] bool hasHeader(const std::string& name) const;
  [[nodiscard]] std::string getHeader(const std::string& name) const; ///< First value or "".
};

// ─── Result types ────────────────────────────────────────────────────────────

struct AuthIdentity {
  std::string subject; ///< Key label (API key) or JWT "sub" claim.
  std::string issuer;  ///< JWT "iss" claim, or empty for API keys.
  /// All string-valued JWT claims, or metadata for API keys.
  std::unordered_map<std::string, std::string> claims;
};

struct AuthResult {
  bool authorized = false;
  int statusCode = 401; ///< 401 = unauthenticated, 403 = forbidden.
  std::string message;
  std::optional<AuthIdentity> identity;
};

// ─── AuthMiddleware ───────────────────────────────────────────────────────────

/// HTTP authentication middleware supporting API key and JWT (HS256).
///
/// Usage in a route handler:
/// @code
///   const AuthResult auth = auth_.authenticate(AuthHttpRequest{req.path, req.headers});
///   if (!auth.authorized) { /* respond 401 */ return; }
///   if (auth.identity) AuthMiddleware::injectIdentity(*auth.identity, eventDoc);
/// @endcode
class AuthMiddleware {
public:
  explicit AuthMiddleware(AuthConfig config);

  /// Validate the incoming request according to the configured mode.
  /// Always returns authorized=true when mode is NONE.
  [[nodiscard]] AuthResult authenticate(const AuthHttpRequest& req) const;

  /// Inject the resolved identity into the lambdaContext sub-object of
  /// @p payload (adds an "identity" member).  Falls back to a top-level
  /// "identity" key if "lambdaContext" is absent.
  static void injectIdentity(const AuthIdentity& id, rapidjson::Document& payload);

  [[nodiscard]] bool isEnabled() const {
    return config_.mode != AuthMode::NONE;
  }
  [[nodiscard]] const AuthConfig& config() const {
    return config_;
  }

private:
  AuthConfig config_;

  [[nodiscard]] bool isPublicPath(const std::string& path) const;

  [[nodiscard]] AuthResult checkApiKey(const AuthHttpRequest& req) const;
  [[nodiscard]] AuthResult checkJwt(const AuthHttpRequest& req) const;

  static std::string extractBearerToken(const AuthHttpRequest& req);

  // Exposed as public static for testing purposes.
public:
  static std::string base64urlDecode(const std::string& input);
  static std::string base64urlEncode(const unsigned char* data, size_t len);

  /// Compute HMAC-SHA256 of @p message with @p secret and return the
  /// result encoded as unpadded base64url.  Requires OpenSSL.
  static std::string hmacSha256Base64url(const std::string& secret, const std::string& message);

  /// Constant-time string comparison (prevents timing attacks).
  static bool constantTimeEqual(const std::string& a, const std::string& b);
};

} // namespace servicegateway
