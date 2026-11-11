// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

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

TEST(JsonObj, SetNullAndSetOrNull) {
  rapidjson::Document doc;
  rdws::utils::json::JsonObj obj(doc.GetAllocator());
  obj.setNull("none")
      .setOrNull("present", std::optional<int>(23))
      .setOrNull("absent", std::optional<int>())
      .setOrNull("text", std::optional<std::string>("v24"));
  const rapidjson::Value value = obj.take();

  ASSERT_TRUE(value.HasMember("none"));
  EXPECT_TRUE(value["none"].IsNull());
  EXPECT_EQ(value["present"].GetInt(), 23);
  ASSERT_TRUE(value.HasMember("absent")); // key kept, value null: the client sees "no value"
  EXPECT_TRUE(value["absent"].IsNull());
  EXPECT_STREQ(value["text"].GetString(), "v24");
}

