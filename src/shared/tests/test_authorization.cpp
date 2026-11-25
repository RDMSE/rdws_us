// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "utils/authorization.h"

#include <gtest/gtest.h>

using rdws::utils::requireAdmin;
using rdws::utils::requireInternal;

namespace {

rapidjson::Document parse(const char* text) {
  rapidjson::Document d;
  d.Parse(text);
  return d;
}

const char* kNoIdentity = R"({"capability":"x"})";
const char* kAdmin =
    R"({"lambdaContext":{"identity":{"subject":"a","claims":{"role":"admin"}}}})";
const char* kOperator =
    R"({"lambdaContext":{"identity":{"subject":"o","claims":{"role":"operator"}}}})";
const char* kNoRole = R"({"lambdaContext":{"identity":{"subject":"o","claims":{}}}})";

} // namespace

TEST(Authorization, AdminOnly) {
  EXPECT_FALSE(requireAdmin(parse(kAdmin)).has_value());
  for (const char* text : {kOperator, kNoRole}) {
    const auto denied = requireAdmin(parse(text));
    ASSERT_TRUE(denied.has_value()) << text;
    EXPECT_EQ((*denied)["statusCode"].GetInt(), 403);
  }
}

TEST(Authorization, AdminLetsCallsWithoutIdentityThrough) {
  EXPECT_FALSE(requireAdmin(parse(kNoIdentity)).has_value()); // dev, or another service
}

TEST(Authorization, InternalOnly) {
  EXPECT_FALSE(requireInternal(parse(kNoIdentity)).has_value());
  for (const char* text : {kAdmin, kOperator, kNoRole}) {
    const auto denied = requireInternal(parse(text));
    ASSERT_TRUE(denied.has_value()) << text;
    EXPECT_EQ((*denied)["statusCode"].GetInt(), 403);
  }
}
