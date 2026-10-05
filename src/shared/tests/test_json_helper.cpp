#include "utils/json_helper.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

using rdws::utils::json::getActorClaim;
using rdws::utils::json::hasActorIdentity;

namespace {

rapidjson::Document parse(const char* json) {
  rapidjson::Document doc;
  doc.Parse(json);
  return doc;
}

} // namespace

TEST(ActorIdentity, NoLambdaContext_NoIdentity) {
  const auto req = parse(R"({"device_id":"12"})");
  EXPECT_FALSE(hasActorIdentity(req));
  EXPECT_EQ(getActorClaim(req, "role"), std::nullopt);
}

TEST(ActorIdentity, LambdaContextWithoutIdentity_NoIdentity) {
  const auto req = parse(R"({"lambdaContext":{"requestId":"r1"}})");
  EXPECT_FALSE(hasActorIdentity(req));
}

TEST(ActorIdentity, JwtClaims_RoleExtracted) {
  const auto req = parse(
      R"({"lambdaContext":{"identity":{"subject":"1","issuer":"rdws",
          "claims":{"username":"rdias","role":"admin"}}}})");
  EXPECT_TRUE(hasActorIdentity(req));
  EXPECT_EQ(getActorClaim(req, "role"), "admin");
}

TEST(ActorIdentity, ApiKeyIdentityWithoutClaims_ClaimAbsent) {
  const auto req =
      parse(R"({"lambdaContext":{"identity":{"subject":"my-service","issuer":"","claims":{}}}})");
  EXPECT_TRUE(hasActorIdentity(req));
  EXPECT_EQ(getActorClaim(req, "role"), std::nullopt);
}
