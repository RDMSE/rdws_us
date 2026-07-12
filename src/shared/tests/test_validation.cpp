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
