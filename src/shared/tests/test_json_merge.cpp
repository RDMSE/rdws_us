#include "utils/json_merge.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

using rdws::utils::json::mergePatch;

namespace {

// Parse both sides and compare as JSON values (not as string — key order should
// not matter for the test).
void expectJsonEq(const std::string& actual, const std::string& expected) {
  rapidjson::Document actualDoc;
  ASSERT_FALSE(actualDoc.Parse(actual.c_str()).HasParseError()) << "actual não é JSON válido: " << actual;

  rapidjson::Document expectedDoc;
  ASSERT_FALSE(expectedDoc.Parse(expected.c_str()).HasParseError()) << "expected não é JSON válido: " << expected;

  EXPECT_EQ(actualDoc, expectedDoc) << "actual=" << actual << " expected=" << expected;
}

} // namespace

TEST(JsonMergeTest, NewKey_IsAdded) {
  const auto result = mergePatch(R"({"a":1})", R"({"b":2})");
  expectJsonEq(result, R"({"a":1,"b":2})");
}

TEST(JsonMergeTest, ExistingKey_IsOverwritten) {
  const auto result = mergePatch(R"({"a":1,"b":2})", R"({"b":99})");
  expectJsonEq(result, R"({"a":1,"b":99})");
}

TEST(JsonMergeTest, UnmentionedKey_IsPreserved) {
  const auto result = mergePatch(R"({"a":1,"b":2,"c":3})", R"({"b":99})");
  expectJsonEq(result, R"({"a":1,"b":99,"c":3})");
}

TEST(JsonMergeTest, NullValue_RemovesKey) {
  const auto result = mergePatch(R"({"a":1,"b":2})", R"({"b":null})");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, NullValue_OnAbsentKey_IsNoOp) {
  const auto result = mergePatch(R"({"a":1})", R"({"missing":null})");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, MixedAddOverwriteRemove_InSinglePatch) {
  const auto result = mergePatch(R"({"a":1,"b":2,"c":3})", R"({"b":99,"c":null,"d":4})");
  expectJsonEq(result, R"({"a":1,"b":99,"d":4})");
}

TEST(JsonMergeTest, EmptyPatch_ReturnsBaseUnchanged) {
  const auto result = mergePatch(R"({"a":1,"b":2})", R"({})");
  expectJsonEq(result, R"({"a":1,"b":2})");
}

TEST(JsonMergeTest, EmptyBase_TreatedAsEmptyObject) {
  const auto result = mergePatch("", R"({"a":1})");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, InvalidBase_TreatedAsEmptyObject) {
  const auto result = mergePatch("not json", R"({"a":1})");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, InvalidPatch_ReturnsBaseUnchanged) {
  const auto result = mergePatch(R"({"a":1})", "not json");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, NonObjectPatch_ReturnsBaseUnchanged) {
  const auto result = mergePatch(R"({"a":1})", R"([1,2,3])");
  expectJsonEq(result, R"({"a":1})");
}

TEST(JsonMergeTest, NestedObjectValue_ReplacedWhole_NotDeepMerged) {
  // Merge is only one level (documented in the header) — nested object is replaced
  // entirely, not deep merged.
  const auto result = mergePatch(R"({"nested":{"x":1,"y":2}})", R"({"nested":{"y":99}})");
  expectJsonEq(result, R"({"nested":{"y":99}})");
}

TEST(JsonMergeTest, RealDeviceConfigScenario_PartialUpdatePreservesOtherFields) {
  const auto step1 = mergePatch("{}", R"({"sampling_interval_s":30,"report_interval_s":120})");
  expectJsonEq(step1, R"({"sampling_interval_s":30,"report_interval_s":120})");

  const auto step2 = mergePatch(step1, R"({"report_interval_s":60})");
  expectJsonEq(step2, R"({"sampling_interval_s":30,"report_interval_s":60})");

  const auto step3 = mergePatch(step2, R"({"sampling_interval_s":null})");
  expectJsonEq(step3, R"({"report_interval_s":60})");
}
