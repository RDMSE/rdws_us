#include "senml/senml.h"

#include <gtest/gtest.h>

using rdws::senml::parse;

namespace {

constexpr double kNow = 1791040000; // arbitrary absolute "current time"

} // namespace

// The example pack from Plano_Telemetria.md (D1).
TEST(SenmlParse, PlanExample_ResolvesBaseFields) {
  const auto result = parse(R"([
    {"bn":"12/", "bt":1791035100, "n":"seq", "v":812},
    {"n":"31", "u":"Cel", "v":25.4, "fl_":2},
    {"n":"32", "u":"%RH", "v":62.1},
    {"n":"33", "u":"Pa",  "v":98780},
    {"n":"rssi", "v":-67},
    {"n":"31", "t":600, "v":25.1}
  ])",
                            kNow);
  ASSERT_TRUE(result) << result.error();
  const auto& recs = *result;
  ASSERT_EQ(recs.size(), 6U);

  EXPECT_EQ(recs[0].name, "12/seq");
  EXPECT_DOUBLE_EQ(recs[0].time, 1791035100);
  EXPECT_DOUBLE_EQ(*recs[0].value, 812); // integer accepted as a number

  EXPECT_EQ(recs[1].name, "12/31");
  EXPECT_EQ(recs[1].unit, "Cel");
  EXPECT_EQ(recs[1].flags, 2);

  EXPECT_DOUBLE_EQ(*recs[3].value, 98780);
  EXPECT_EQ(recs[4].unit, ""); // diagnostics carry no unit
  EXPECT_EQ(recs[4].flags, 0);

  EXPECT_EQ(recs[5].name, "12/31");
  EXPECT_DOUBLE_EQ(recs[5].time, 1791035700);
}

TEST(SenmlParse, BaseFieldsPersistUntilRedefined) {
  const auto result = parse(R"([
    {"bn":"12/", "bt":1791035100, "bu":"Cel", "bv":20, "n":"31", "v":1.5},
    {"n":"31", "t":600, "v":2},
    {"bn":"12/child/", "n":"9", "u":"V", "v":3}
  ])",
                            kNow);
  ASSERT_TRUE(result) << result.error();
  const auto& recs = *result;
  EXPECT_EQ(recs[1].unit, "Cel");
  EXPECT_DOUBLE_EQ(*recs[1].value, 22);
  EXPECT_DOUBLE_EQ(recs[1].time, 1791035700);
  EXPECT_EQ(recs[2].name, "12/child/9");
  EXPECT_EQ(recs[2].unit, "V");
  EXPECT_DOUBLE_EQ(recs[2].time, 1791035100);
}

TEST(SenmlParse, SmallTimesAreRelativeToNow) {
  const auto result = parse(R"([{"n":"x", "t":-60, "v":1}, {"n":"y", "v":2}])", kNow);
  ASSERT_TRUE(result) << result.error();
  EXPECT_DOUBLE_EQ((*result)[0].time, kNow - 60);
  EXPECT_DOUBLE_EQ((*result)[1].time, kNow);
}

TEST(SenmlParse, BoolAndStringValues) {
  const auto result =
      parse(R"([{"n":"fs_reformat", "vb":true}, {"n":"fw", "vs":"1.2.3"}])", kNow);
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(*(*result)[0].boolValue);
  EXPECT_FALSE((*result)[0].value.has_value());
  EXPECT_EQ(*(*result)[1].stringValue, "1.2.3");
}

TEST(SenmlParse, UnknownPlainFieldIgnored) {
  const auto result = parse(R"([{"n":"x", "v":1, "ut":30}])", kNow);
  ASSERT_TRUE(result) << result.error();
}

TEST(SenmlParse, EmptyPackIsValid) {
  const auto result = parse("[]", kNow);
  ASSERT_TRUE(result) << result.error();
  EXPECT_TRUE(result->empty());
}

TEST(SenmlParse, UnknownMustUnderstandField_Rejected) {
  const auto result = parse(R"([{"n":"x", "v":1, "foo_":1}])", kNow);
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("foo_"), std::string::npos);
}

TEST(SenmlParse, MalformedPacks_Rejected) {
  EXPECT_FALSE(parse("{not json", kNow));
  EXPECT_FALSE(parse(R"({"n":"x","v":1})", kNow));          // not an array
  EXPECT_FALSE(parse(R"([1])", kNow));                       // record not an object
  EXPECT_FALSE(parse(R"([{"n":"x"}])", kNow));               // no value
  EXPECT_FALSE(parse(R"([{"n":"x","v":1,"vb":true}])", kNow)); // two values
  EXPECT_FALSE(parse(R"([{"n":"x","v":"1"}])", kNow));       // v not a number
  EXPECT_FALSE(parse(R"([{"v":1}])", kNow));                 // no name at all
  EXPECT_FALSE(parse(R"([{"n":"-x","v":1}])", kNow));        // must start alphanumeric
  EXPECT_FALSE(parse(R"([{"n":"a b","v":1}])", kNow));       // invalid char
  EXPECT_FALSE(parse(R"([{"bver":11,"n":"x","v":1}])", kNow)); // newer SenML version
}

TEST(SenmlParse, InvalidFlags_Rejected) {
  EXPECT_FALSE(parse(R"([{"n":"x","v":1,"fl_":-1}])", kNow));
  EXPECT_FALSE(parse(R"([{"n":"x","v":1,"fl_":70000}])", kNow));
  EXPECT_FALSE(parse(R"([{"n":"x","v":1,"fl_":"2"}])", kNow));
}
