// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "utils/validation.h"

#include <gtest/gtest.h>

using rdws::utils::isNumericId;

TEST(IdValidationTest, RejectsEmptyValue) { EXPECT_FALSE(isNumericId("")); }

TEST(IdValidationTest, RejectsNonDigitCharacters) {
  EXPECT_FALSE(isNumericId("12a3"));
  EXPECT_FALSE(isNumericId("-123"));
}

TEST(IdValidationTest, AcceptsValidBigintRangeValues) {
  EXPECT_TRUE(isNumericId("0"));
  EXPECT_TRUE(isNumericId("42"));
  EXPECT_TRUE(isNumericId("9223372036854775807"));
}

TEST(IdValidationTest, RejectsValuesAboveBigintMax) {
  EXPECT_FALSE(isNumericId("9223372036854775808"));
  EXPECT_FALSE(isNumericId("10000000000000000000"));
}

TEST(IdValidationTest, AcceptsLongValueWhenLeadingZerosKeepRangeValid) {
  EXPECT_TRUE(isNumericId("000000000000000000001"));
}

using rdws::utils::isUuid;

TEST(UuidValidationTest, AcceptsCanonicalFormEitherCase) {
  EXPECT_TRUE(isUuid("3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9b"));
  EXPECT_TRUE(isUuid("3F2B8C1E-9D4A-4B7E-8A21-0C5D6E7F8A9B"));
}

TEST(UuidValidationTest, RejectsNumericIdsAndMalformedValues) {
  EXPECT_FALSE(isUuid(""));
  EXPECT_FALSE(isUuid("12"));
  EXPECT_FALSE(isUuid("3f2b8c1e9d4a4b7e8a210c5d6e7f8a9b"));     // no dashes
  EXPECT_FALSE(isUuid("3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9"));   // one short
  EXPECT_FALSE(isUuid("3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9bc")); // one long
  EXPECT_FALSE(isUuid("3f2b8c1e-9d4a-4b7e-8a21-0c5d6e7f8a9g"));  // not hex
  EXPECT_FALSE(isUuid("3f2b8c1e-9d4a-4b7e-8a210-c5d6e7f8a9b"));  // dash misplaced
}
