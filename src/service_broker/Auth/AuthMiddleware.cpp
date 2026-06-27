#include "AuthMiddleware.h"

#include <algorithm>
#include <chrono>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <rapidjson/document.h>
#include <string_view>

namespace servicegateway {

// ─── AuthHttpRequest helpers ──────────────────────────────────────────────────

bool AuthHttpRequest::hasHeader(const std::string& name) const {
  return headers.contains(name);
}

std::string AuthHttpRequest::getHeader(const std::string& name) const {
  const auto it = headers.find(name);
  return it != headers.end() ? it->second : "";
}

// ─── Construction ─────────────────────────────────────────────────────────────

AuthMiddleware::AuthMiddleware(AuthConfig config) : config_(std::move(config)) {}

// ─── Public interface ────────────────────────────────────────────────────────

AuthResult AuthMiddleware::authenticate(const AuthHttpRequest& req) const {
  if (config_.mode == AuthMode::NONE || isPublicPath(req.path)) {
    return {.authorized = true, .statusCode = 200, .message = "ok", .identity = std::nullopt};
  }

  switch (config_.mode) {
    case AuthMode::API_KEY:
      return checkApiKey(req);
    case AuthMode::JWT:
      return checkJwt(req);
    default:
      return {.authorized = true, .statusCode = 200, .message = "ok", .identity = std::nullopt};
  }
}

void AuthMiddleware::injectIdentity(const AuthIdentity& id, rapidjson::Document& payload) {
  auto& alloc = payload.GetAllocator();

  rapidjson::Value claimsObj(rapidjson::kObjectType);
  for (const auto& [k, v] : id.claims) {
    claimsObj.AddMember(rapidjson::Value(k.c_str(), alloc), rapidjson::Value(v.c_str(), alloc),
                        alloc);
  }

  rapidjson::Value identityObj(rapidjson::kObjectType);
  identityObj.AddMember("subject", rapidjson::Value(id.subject.c_str(), alloc), alloc);
  identityObj.AddMember("issuer", rapidjson::Value(id.issuer.c_str(), alloc), alloc);
  identityObj.AddMember("claims", claimsObj, alloc);

  if (payload.HasMember("lambdaContext") && payload["lambdaContext"].IsObject()) {
    payload["lambdaContext"].AddMember("identity", identityObj, alloc);
  } else {
    payload.AddMember("identity", identityObj, alloc);
  }
}

// ─── API key ──────────────────────────────────────────────────────────────────

AuthResult AuthMiddleware::checkApiKey(const AuthHttpRequest& req) const {
  std::string key;

  // X-API-Key header takes precedence over Authorization: Bearer.
  if (req.hasHeader("X-API-Key")) {
    key = req.getHeader("X-API-Key");
  } else {
    key = extractBearerToken(req);
  }

  if (key.empty()) {
    return {.authorized = false,
            .statusCode = 401,
            .message = "Missing API key (X-API-Key header or Authorization: Bearer)"};
  }

  const auto it = config_.apiKeys.find(key);
  if (it == config_.apiKeys.end()) {
    return {.authorized = false, .statusCode = 401, .message = "Invalid API key"};
  }

  AuthIdentity id;
  id.subject = it->second.empty() ? key : it->second;
  return {.authorized = true, .statusCode = 200, .message = "ok", .identity = id};
}

// ─── JWT (HS256) ──────────────────────────────────────────────────────────────

AuthResult AuthMiddleware::checkJwt(const AuthHttpRequest& req) const {
  const std::string token = extractBearerToken(req);
  if (token.empty()) {
    return {
        .authorized = false, .statusCode = 401, .message = "Missing Authorization: Bearer token"};
  }

  // ── Split header.payload.signature ────────────────────────────────────
  const auto dot1 = token.find('.');
  if (dot1 == std::string::npos) {
    return {.authorized = false, .statusCode = 401, .message = "Malformed JWT: missing first dot"};
  }

  const auto dot2 = token.find('.', dot1 + 1);
  if (dot2 == std::string::npos) {
    return {.authorized = false, .statusCode = 401, .message = "Malformed JWT: missing second dot"};
  }

  const std::string headerB64 = token.substr(0, dot1);
  const std::string payloadB64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
  const std::string sigB64 = token.substr(dot2 + 1);

  // ── Verify algorithm declared in header ───────────────────────────────
  {
    const std::string headerJson = base64urlDecode(headerB64);
    rapidjson::Document hdr;
    hdr.Parse(headerJson.c_str());
    if (hdr.HasParseError() || !hdr.IsObject()) {
      return {.authorized = false, .statusCode = 401, .message = "Malformed JWT header"};
    }
    if (!hdr.HasMember("alg") || !hdr["alg"].IsString() ||
        std::string(hdr["alg"].GetString()) != "HS256") {
      return {.authorized = false,
              .statusCode = 401,
              .message = "Unsupported JWT algorithm (only HS256 accepted)"};
    }
  }

  // ── Verify HMAC-SHA256 signature ──────────────────────────────────────
  const std::string signingInput = headerB64 + "." + payloadB64;
  const std::string computedSig = hmacSha256Base64url(config_.jwtSecret, signingInput);

  if (!constantTimeEqual(computedSig, sigB64)) {
    return {.authorized = false, .statusCode = 401, .message = "Invalid JWT signature"};
  }

  // ── Parse payload claims ──────────────────────────────────────────────
  const std::string payloadJson = base64urlDecode(payloadB64);
  rapidjson::Document doc;
  doc.Parse(payloadJson.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return {.authorized = false, .statusCode = 401, .message = "Malformed JWT payload"};
  }

  // ── Validate expiry ───────────────────────────────────────────────────
  if (doc.HasMember("exp")) {
    const auto& expVal = doc["exp"];
    const int64_t exp = expVal.IsInt64()    ? expVal.GetInt64()
                        : expVal.IsInt()    ? expVal.GetInt()
                        : expVal.IsDouble() ? static_cast<int64_t>(expVal.GetDouble())
                                            : -1;
    if (exp >= 0) {
      const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      if (now > exp) {
        return {.authorized = false, .statusCode = 401, .message = "JWT token has expired"};
      }
    }
  }

  // ── Validate not-before ───────────────────────────────────────────────
  if (doc.HasMember("nbf")) {
    const auto& nbfVal = doc["nbf"];
    const int64_t nbf = nbfVal.IsInt64()    ? nbfVal.GetInt64()
                        : nbfVal.IsInt()    ? nbfVal.GetInt()
                        : nbfVal.IsDouble() ? static_cast<int64_t>(nbfVal.GetDouble())
                                            : 0;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now < nbf) {
      return {.authorized = false, .statusCode = 401, .message = "JWT not yet valid (nbf)"};
    }
  }

  // ── Validate issuer ───────────────────────────────────────────────────
  if (!config_.jwtIssuer.empty()) {
    if (!doc.HasMember("iss") || !doc["iss"].IsString() ||
        doc["iss"].GetString() != config_.jwtIssuer) {
      return {.authorized = false, .statusCode = 401, .message = "JWT issuer mismatch"};
    }
  }

  // ── Validate audience ─────────────────────────────────────────────────
  if (!config_.jwtAudience.empty()) {
    bool audOk = false;
    if (doc.HasMember("aud")) {
      const auto& aud = doc["aud"];
      if (aud.IsString()) {
        audOk = (aud.GetString() == config_.jwtAudience);
      } else if (aud.IsArray()) {
        for (const auto& a : aud.GetArray()) {
          if (a.IsString() && a.GetString() == config_.jwtAudience) {
            audOk = true;
            break;
          }
        }
      }
    }
    if (!audOk) {
      return {.authorized = false, .statusCode = 401, .message = "JWT audience mismatch"};
    }
  }

  // ── Build identity ────────────────────────────────────────────────────
  AuthIdentity id;
  if (doc.HasMember("sub") && doc["sub"].IsString()) {
    id.subject = doc["sub"].GetString();
  }
  if (doc.HasMember("iss") && doc["iss"].IsString()) {
    id.issuer = doc["iss"].GetString();
  }

  // Collect all string-valued claims as identity metadata.
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string name = it->name.GetString();
    if (it->value.IsString()) {
      id.claims[name] = it->value.GetString();
    } else if (it->value.IsInt64()) {
      id.claims[name] = std::to_string(it->value.GetInt64());
    } else if (it->value.IsInt()) {
      id.claims[name] = std::to_string(it->value.GetInt());
    } else if (it->value.IsDouble()) {
      id.claims[name] = std::to_string(static_cast<int64_t>(it->value.GetDouble()));
    }
  }

  return {.authorized = true, .statusCode = 200, .message = "ok", .identity = id};
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

bool AuthMiddleware::isPublicPath(const std::string& path) const {
  for (const auto& pub : config_.publicPaths) {
    if (path == pub || path.starts_with(pub + "/") || path.starts_with(pub + "?")) {
      return true;
    }
  }
  return false;
}

std::string AuthMiddleware::extractBearerToken(const AuthHttpRequest& req) {
  if (!req.hasHeader("Authorization")) {
    return {};
  }
  const std::string auth = req.getHeader("Authorization");
  constexpr std::string_view prefix = "Bearer ";
  if (auth.size() <= prefix.size() || !auth.starts_with(prefix)) {
    return {};
  }
  return auth.substr(prefix.size());
}

// ─── Crypto / encoding ────────────────────────────────────────────────────────

std::string AuthMiddleware::base64urlDecode(const std::string& input) {
  // Convert base64url alphabet → standard base64 and add padding.
  std::string b64 = input;
  for (char& c : b64) {
    if (c == '-') {
      c = '+';
    } else if (c == '_') {
      c = '/';
    }
  }
  while (b64.size() % 4 != 0) {
    b64 += '=';
  }

  // Decode using a character lookup table.
  static constexpr std::string_view chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string result;
  result.reserve(b64.size() * 3 / 4);

  int val = 0;
  int bits = -8;
  for (const unsigned char c : b64) {
    if (c == '=') {
      break;
    }
    const auto pos = chars.find(static_cast<char>(c));
    if (pos == std::string_view::npos) {
      continue;
    }
    val = (val << 6) + static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      result += static_cast<char>((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return result;
}

std::string AuthMiddleware::base64urlEncode(const unsigned char* data, const size_t len) {
  if (len == 0) {
    return {};
  }

  static constexpr std::string_view b64chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string result;
  result.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    const unsigned int b0 = data[i];
    const unsigned int b1 = (i + 1 < len) ? static_cast<unsigned int>(data[i + 1]) : 0U;
    const unsigned int b2 = (i + 2 < len) ? static_cast<unsigned int>(data[i + 2]) : 0U;
    const size_t remaining = len - i;

    // Always emit first two base64 chars (cover all bits of b0 and top bits of b1).
    result += b64chars[(b0 >> 2) & 0x3F];
    result += b64chars[((b0 & 3) << 4) | ((b1 >> 4) & 0xF)];
    // Third char only when there is a real second byte.
    if (remaining > 1) {
      result += b64chars[((b1 & 0xF) << 2) | ((b2 >> 6) & 3)];
    }
    // Fourth char only when there is a real third byte.
    if (remaining > 2) {
      result += b64chars[b2 & 0x3F];
    }
  }

  // Convert standard base64 alphabet → base64url (no padding characters added).
  for (char& c : result) {
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    }
  }
  return result;
}

std::string AuthMiddleware::hmacSha256Base64url(const std::string& secret,
                                                const std::string& message) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
  unsigned int digestLen = 0;

  HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(message.data()), static_cast<int>(message.size()),
       digest.data(), &digestLen);

  return base64urlEncode(digest.data(), digestLen);
}

bool AuthMiddleware::constantTimeEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  int diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
  }
  return diff == 0;
}

} // namespace servicegateway
