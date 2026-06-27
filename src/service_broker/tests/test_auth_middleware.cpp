#include "../Auth/AuthMiddleware.h"

#include <chrono>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

namespace servicegateway {

// ─── JWT token generator (test utility) ─────────────────────────────────────

namespace {

std::string jsonToBase64url(const std::string& json) {
  return AuthMiddleware::base64urlEncode(reinterpret_cast<const unsigned char*>(json.data()),
                                         json.size());
}

/// Build a valid HS256 JWT with the given payload claims JSON object string.
std::string makeJwt(const std::string& secret, const std::string& payloadJson) {
  const std::string headerB64 = jsonToBase64url(R"({"alg":"HS256","typ":"JWT"})");
  const std::string payloadB64 = jsonToBase64url(payloadJson);
  const std::string signingInput = headerB64 + "." + payloadB64;
  const std::string sig = AuthMiddleware::hmacSha256Base64url(secret, signingInput);
  return headerB64 + "." + payloadB64 + "." + sig;
}

/// Unix timestamp 10 years from now — effectively "never expires".
int64_t futureExp() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() +
         315360000; // 10 years
}

/// Unix timestamp 10 years in the past — always expired.
int64_t pastExp() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() -
         315360000;
}

AuthHttpRequest makeRequest(const std::string& path = "/invoke/greet",
                            const std::string& authHeader = "") {
  AuthHttpRequest req;
  req.path = path;
  if (!authHeader.empty()) {
    req.headers.emplace("Authorization", authHeader);
  }
  return req;
}

AuthHttpRequest makeRequestWithApiKeyHeader(const std::string& path, const std::string& key) {
  AuthHttpRequest req;
  req.path = path;
  req.headers.emplace("X-API-Key", key);
  return req;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Encoding helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST(AuthBase64, RoundTrip_ShortString) {
  const std::string original = "Hello, JWT!";
  const std::string encoded = AuthMiddleware::base64urlEncode(
      reinterpret_cast<const unsigned char*>(original.data()), original.size());
  const std::string decoded = AuthMiddleware::base64urlDecode(encoded);
  EXPECT_EQ(decoded, original);
}

TEST(AuthBase64, RoundTrip_EmptyString) {
  const std::string encoded = AuthMiddleware::base64urlEncode(nullptr, 0);
  const std::string decoded = AuthMiddleware::base64urlDecode(encoded);
  EXPECT_EQ(decoded, "");
}

TEST(AuthBase64, NoPaddingCharacters) {
  const std::string input = "any binary data here";
  const std::string encoded = AuthMiddleware::base64urlEncode(
      reinterpret_cast<const unsigned char*>(input.data()), input.size());
  EXPECT_EQ(encoded.find('='), std::string::npos);
  EXPECT_EQ(encoded.find('+'), std::string::npos);
  EXPECT_EQ(encoded.find('/'), std::string::npos);
}

TEST(AuthHmac, KnownVector_HS256) {
  // RFC 4231 HMAC-SHA256 test vector:
  // key  = 0x0b * 20
  // data = "Hi There"
  // expected digest (hex): b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
  const std::string key(20, '\x0b');
  const std::string msg = "Hi There";
  const std::string b64 = AuthMiddleware::hmacSha256Base64url(key, msg);
  // Decode and check the raw bytes.
  const std::string raw = AuthMiddleware::base64urlDecode(b64);
  ASSERT_EQ(raw.size(), 32u);
  EXPECT_EQ(static_cast<unsigned char>(raw[0]), 0xb0u);
  EXPECT_EQ(static_cast<unsigned char>(raw[1]), 0x34u);
  EXPECT_EQ(static_cast<unsigned char>(raw[31]), 0xf7u);
}

TEST(AuthConstantTime, EqualStrings_ReturnsTrue) {
  EXPECT_TRUE(AuthMiddleware::constantTimeEqual("abc", "abc"));
}

TEST(AuthConstantTime, UnequalStrings_ReturnsFalse) {
  EXPECT_FALSE(AuthMiddleware::constantTimeEqual("abc", "abd"));
}

TEST(AuthConstantTime, DifferentLengths_ReturnsFalse) {
  EXPECT_FALSE(AuthMiddleware::constantTimeEqual("abc", "abcd"));
}

// ─────────────────────────────────────────────────────────────────────────────
// AuthMode::NONE — always passes
// ─────────────────────────────────────────────────────────────────────────────

TEST(AuthMiddlewareNone, NoAuth_AlwaysAuthorized) {
  AuthMiddleware auth(AuthConfig{});
  const auto result = auth.authenticate(makeRequest("/invoke/test"));
  EXPECT_TRUE(result.authorized);
  EXPECT_EQ(result.statusCode, 200);
  EXPECT_FALSE(result.identity.has_value());
}

TEST(AuthMiddlewareNone, IsEnabled_ReturnsFalse) {
  AuthMiddleware auth(AuthConfig{});
  EXPECT_FALSE(auth.isEnabled());
}

// ─────────────────────────────────────────────────────────────────────────────
// AuthMode::API_KEY
// ─────────────────────────────────────────────────────────────────────────────

class ApiKeyTest : public ::testing::Test {
protected:
  void SetUp() override {
    AuthConfig cfg;
    cfg.mode = AuthMode::API_KEY;
    cfg.apiKeys["sk_valid"] = "my-service";
    cfg.apiKeys["sk_unlabeled"] = ""; // label = key itself
    auth_ = std::make_unique<AuthMiddleware>(cfg);
  }
  std::unique_ptr<AuthMiddleware> auth_;
};

TEST_F(ApiKeyTest, ValidKey_XApiKeyHeader_Authorized) {
  const auto req = makeRequestWithApiKeyHeader("/invoke/greet", "sk_valid");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
  ASSERT_TRUE(result.identity.has_value());
  EXPECT_EQ(result.identity->subject, "my-service");
}

TEST_F(ApiKeyTest, ValidKey_BearerHeader_Authorized) {
  const auto req = makeRequest("/invoke/greet", "Bearer sk_valid");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
  ASSERT_TRUE(result.identity.has_value());
  EXPECT_EQ(result.identity->subject, "my-service");
}

TEST_F(ApiKeyTest, UnlabeledKey_SubjectIsKeyItself) {
  const auto req = makeRequestWithApiKeyHeader("/invoke/greet", "sk_unlabeled");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
  ASSERT_TRUE(result.identity.has_value());
  EXPECT_EQ(result.identity->subject, "sk_unlabeled");
}

TEST_F(ApiKeyTest, InvalidKey_Returns401) {
  const auto req = makeRequestWithApiKeyHeader("/invoke/greet", "sk_wrong");
  const auto result = auth_->authenticate(req);
  EXPECT_FALSE(result.authorized);
  EXPECT_EQ(result.statusCode, 401);
}

TEST_F(ApiKeyTest, MissingKey_Returns401) {
  const auto req = makeRequest("/invoke/greet");
  const auto result = auth_->authenticate(req);
  EXPECT_FALSE(result.authorized);
  EXPECT_EQ(result.statusCode, 401);
}

TEST_F(ApiKeyTest, XApiKey_TakesPrecedenceOverBearer) {
  // X-API-Key header wins even when Authorization header is also present.
  AuthHttpRequest req;
  req.path = "/invoke/greet";
  req.headers.emplace("X-API-Key", "sk_valid");
  req.headers.emplace("Authorization", "Bearer sk_wrong");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
}

TEST_F(ApiKeyTest, PublicPath_BypassesAuth) {
  const auto req = makeRequest("/health");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
}

TEST_F(ApiKeyTest, PublicPathSubRoute_BypassesAuth) {
  const auto req = makeRequest("/routes/abc-123");
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
}

TEST_F(ApiKeyTest, NonPublicPath_RequiresKey) {
  const auto req = makeRequest("/invoke/greet"); // no header
  const auto result = auth_->authenticate(req);
  EXPECT_FALSE(result.authorized);
}

TEST_F(ApiKeyTest, IsEnabled_ReturnsTrue) {
  EXPECT_TRUE(auth_->isEnabled());
}

// ─────────────────────────────────────────────────────────────────────────────
// AuthMode::JWT
// ─────────────────────────────────────────────────────────────────────────────

class JwtTest : public ::testing::Test {
protected:
  static constexpr const char* kSecret = "super-secret-key-for-tests";
  static constexpr const char* kIssuer = "test-issuer";
  static constexpr const char* kAudience = "test-audience";

  void SetUp() override {
    AuthConfig cfg;
    cfg.mode = AuthMode::JWT;
    cfg.jwtSecret = kSecret;
    cfg.jwtIssuer = kIssuer;
    cfg.jwtAudience = kAudience;
    auth_ = std::make_unique<AuthMiddleware>(cfg);
  }

  std::string validToken(const std::string& subject = "alice@example.com",
                         const std::string& extra = "") const {
    std::string payload = R"({"sub":")" + subject + R"(","iss":")" + kIssuer + R"(","aud":")" +
                          kAudience + R"(","exp":)" + std::to_string(futureExp()) + extra + "}";
    return makeJwt(kSecret, payload);
  }

  std::unique_ptr<AuthMiddleware> auth_;
};

TEST_F(JwtTest, ValidToken_Authorized) {
  const auto req = makeRequest("/invoke/greet", "Bearer " + validToken());
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
  ASSERT_TRUE(result.identity.has_value());
  EXPECT_EQ(result.identity->subject, "alice@example.com");
  EXPECT_EQ(result.identity->issuer, kIssuer);
}

TEST_F(JwtTest, ValidToken_ClaimsExtracted) {
  const std::string token =
      makeJwt(kSecret, R"({"sub":"bob","iss":")" + std::string(kIssuer) + R"(","aud":")" +
                           std::string(kAudience) + R"(","exp":)" + std::to_string(futureExp()) +
                           R"(,"role":"admin"})");
  const auto req = makeRequest("/invoke/greet", "Bearer " + token);
  const auto result = auth_->authenticate(req);
  EXPECT_TRUE(result.authorized);
  ASSERT_TRUE(result.identity.has_value());
  EXPECT_EQ(result.identity->claims.at("role"), "admin");
}

TEST_F(JwtTest, WrongSecret_Returns401) {
  const std::string badToken =
      makeJwt("wrong-secret", R"({"sub":"alice","iss":")" + std::string(kIssuer) + R"(","aud":")" +
                                  std::string(kAudience) + R"(","exp":)" +
                                  std::to_string(futureExp()) + "}");
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer " + badToken));
  EXPECT_FALSE(result.authorized);
  EXPECT_EQ(result.statusCode, 401);
}

TEST_F(JwtTest, ExpiredToken_Returns401) {
  const std::string token = makeJwt(kSecret, R"({"sub":"alice","iss":")" + std::string(kIssuer) +
                                                 R"(","aud":")" + std::string(kAudience) +
                                                 R"(","exp":)" + std::to_string(pastExp()) + "}");
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer " + token));
  EXPECT_FALSE(result.authorized);
  EXPECT_NE(result.message.find("expired"), std::string::npos);
}

TEST_F(JwtTest, WrongIssuer_Returns401) {
  const std::string token =
      makeJwt(kSecret, R"({"sub":"alice","iss":"other-issuer","aud":")" + std::string(kAudience) +
                           R"(","exp":)" + std::to_string(futureExp()) + "}");
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer " + token));
  EXPECT_FALSE(result.authorized);
  EXPECT_NE(result.message.find("issuer"), std::string::npos);
}

TEST_F(JwtTest, WrongAudience_Returns401) {
  const std::string token =
      makeJwt(kSecret, R"({"sub":"alice","iss":")" + std::string(kIssuer) +
                           R"(","aud":"other-aud","exp":)" + std::to_string(futureExp()) + "}");
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer " + token));
  EXPECT_FALSE(result.authorized);
  EXPECT_NE(result.message.find("audience"), std::string::npos);
}

TEST_F(JwtTest, MissingToken_Returns401) {
  const auto result = auth_->authenticate(makeRequest("/invoke/x"));
  EXPECT_FALSE(result.authorized);
  EXPECT_EQ(result.statusCode, 401);
}

TEST_F(JwtTest, MalformedToken_NoDots_Returns401) {
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer notavalidtoken"));
  EXPECT_FALSE(result.authorized);
}

TEST_F(JwtTest, PublicPath_BypassesJwtCheck) {
  const auto result = auth_->authenticate(makeRequest("/health"));
  EXPECT_TRUE(result.authorized);
}

TEST_F(JwtTest, NoIssuerCheck_WhenIssuerConfigEmpty) {
  AuthConfig cfg;
  cfg.mode = AuthMode::JWT;
  cfg.jwtSecret = kSecret;
  cfg.jwtIssuer = "";   // skip issuer check
  cfg.jwtAudience = ""; // skip audience check
  AuthMiddleware auth(cfg);

  const std::string token =
      makeJwt(kSecret, R"({"sub":"charlie","exp":)" + std::to_string(futureExp()) + "}");
  const auto result = auth.authenticate(makeRequest("/invoke/x", "Bearer " + token));
  EXPECT_TRUE(result.authorized);
  EXPECT_EQ(result.identity->subject, "charlie");
}

TEST_F(JwtTest, AudienceArray_Matches) {
  // JWT where aud is a JSON array containing the expected audience.
  const std::string token = makeJwt(
      kSecret, R"({"sub":"alice","iss":")" + std::string(kIssuer) + R"(","aud":["other",")" +
                   std::string(kAudience) + R"("],"exp":)" + std::to_string(futureExp()) + "}");
  const auto result = auth_->authenticate(makeRequest("/invoke/x", "Bearer " + token));
  EXPECT_TRUE(result.authorized);
}

// ─────────────────────────────────────────────────────────────────────────────
// Identity injection
// ─────────────────────────────────────────────────────────────────────────────

TEST(AuthIdentityInjection, InjectsIntoLambdaContext) {
  rapidjson::Document payload;
  payload.SetObject();
  auto& alloc = payload.GetAllocator();
  rapidjson::Value ctx(rapidjson::kObjectType);
  ctx.AddMember("requestId", "req-1", alloc);
  payload.AddMember("lambdaContext", ctx, alloc);

  AuthIdentity id;
  id.subject = "alice";
  id.issuer = "my-issuer";
  id.claims["role"] = "admin";

  AuthMiddleware::injectIdentity(id, payload);

  ASSERT_TRUE(payload["lambdaContext"].HasMember("identity"));
  const auto& identity = payload["lambdaContext"]["identity"];
  EXPECT_STREQ(identity["subject"].GetString(), "alice");
  EXPECT_STREQ(identity["issuer"].GetString(), "my-issuer");
  EXPECT_STREQ(identity["claims"]["role"].GetString(), "admin");
}

TEST(AuthIdentityInjection, FallsBackToTopLevel_WhenNoLambdaContext) {
  rapidjson::Document payload;
  payload.SetObject();

  AuthIdentity id;
  id.subject = "bob";
  id.issuer = "";

  AuthMiddleware::injectIdentity(id, payload);

  ASSERT_TRUE(payload.HasMember("identity"));
  EXPECT_STREQ(payload["identity"]["subject"].GetString(), "bob");
}

} // namespace servicegateway
