/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <gtest/gtest.h>
#include "sql/ob_sql_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/rewrite/ob_query_range_define.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
int match(DatumParameter &value, PreCalcExprExpectResult expected, bool &matches)
{
  ObArenaAllocator allocator;
  std::unique_ptr<ObExecContext> context(new ObExecContext(allocator));
  ObPreCalcExprConstraint constraint(allocator); constraint.expect_result_ = expected;
  return constraint.check_is_match(value, *context, matches);
}
void expect_match(DatumParameter &value, PreCalcExprExpectResult expected, bool matches)
{
  bool actual = !matches;
  ASSERT_EQ(OB_SUCCESS, match(value, expected, actual));
  EXPECT_EQ(matches, actual) << "constraint=" << expected;
}
}

TEST(DatumParameterConstraints, null_boolean_never_reads_a_payload)
{
  DatumParameter value; ObObjMeta meta; meta.set_int(); value.set_type_meta(meta);
  value.datum_.ptr_ = nullptr; value.datum_.set_null();
  expect_match(value, PRE_CALC_RESULT_NULL, true);
  expect_match(value, PRE_CALC_RESULT_NOT_NULL, false);
  expect_match(value, PRE_CALC_RESULT_TRUE, false);
  expect_match(value, PRE_CALC_RESULT_FALSE, false);
  expect_match(value, PRE_CALC_PRECISE, false);
  expect_match(value, PRE_CALC_NOT_PRECISE, true);
  expect_match(value, PRE_CALC_RESULT_NO_WILDCARD, true);
}

TEST(DatumParameterConstraints, signed_boolean_zero_and_nonzero)
{
  char bytes[8]; DatumParameter value; ObObjMeta meta; meta.set_int();
  value.set_type_meta(meta); value.datum_.ptr_ = bytes;
  for (int64_t n : {-17LL, 0LL, 1LL, 9223372036854775807LL}) {
    value.datum_.set_int(n);
    expect_match(value, PRE_CALC_RESULT_NULL, false);
    expect_match(value, PRE_CALC_RESULT_NOT_NULL, true);
    expect_match(value, PRE_CALC_RESULT_TRUE, n != 0);
    expect_match(value, PRE_CALC_RESULT_FALSE, n == 0);
  }
}

TEST(DatumParameterConstraints, empty_null_and_nonempty_string_are_distinct)
{
  DatumParameter value; ObObjMeta meta; meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN); value.set_type_meta(meta);
  value.datum_.set_string(nullptr, 0);
  expect_match(value, PRE_CALC_RESULT_NULL, false);
  expect_match(value, PRE_CALC_PRECISE, true);
  expect_match(value, PRE_CALC_RESULT_NO_WILDCARD, true);
  value.datum_.set_null();
  expect_match(value, PRE_CALC_RESULT_NULL, true);
  expect_match(value, PRE_CALC_PRECISE, false);
  expect_match(value, PRE_CALC_NOT_PRECISE, true);
}

TEST(DatumParameterConstraints, wildcard_check_handles_binary_nul_and_unicode)
{
  DatumParameter value; ObObjMeta meta; meta.set_varbinary(); value.set_type_meta(meta);
  struct Case { const char *bytes; uint32_t length; bool no_wildcard; };
  const Case cases[] = {{"plain", 5, true}, {"a%b", 3, false}, {"a_b", 3, false},
                        {"a\\b", 3, false}, {"\0%", 2, false}, {"\0x", 2, true},
                        {"中文", 6, true}};
  for (const auto &c : cases) {
    value.datum_.set_string(c.bytes, c.length);
    expect_match(value, PRE_CALC_RESULT_NO_WILDCARD, c.no_wildcard);
  }
}

TEST(DatumParameterConstraints, lob_null_and_value_preserve_old_constraint_results)
{
  DatumParameter value; ObObjMeta meta; meta.set_blob(); meta.set_has_lob_header();
  value.set_type_meta(meta);
  value.datum_.set_null();
  expect_match(value, PRE_CALC_RESULT_NO_WILDCARD, true);
  value.datum_.set_string("", 0);
  expect_match(value, PRE_CALC_RESULT_NO_WILDCARD, false);
}

// A literal without wildcards still needs the LIKE filter for trailing-space
// semantics; an empty string is the existing special precise case.
TEST(DatumParameterConstraints, like_precision_keeps_prefix_and_gap_rules)
{
  DatumParameter value; ObObjMeta meta; meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN); value.set_type_meta(meta);
  struct Case { const char *pattern; bool precise; };
  for (const Case &c : {Case{"literal", false}, Case{"prefix%", true},
                         Case{"gap%tail", false}, Case{"a_b", false}}) {
    value.datum_.set_string(c.pattern, strlen(c.pattern));
    expect_match(value, PRE_CALC_PRECISE, c.precise);
    expect_match(value, PRE_CALC_NOT_PRECISE, !c.precise);
  }
}

TEST(DatumParameterConstraints, legacy_input_keeps_full_metadata_without_reverse_object)
{
  ObArenaAllocator allocator;
  ObObjParam source; source.set_null();
  ObObjMeta declared; declared.set_varbinary();
  declared.set_collation_level(CS_LEVEL_COERCIBLE); source.set_param_meta(declared);
  ObAccuracy accuracy; accuracy.set_accuracy(0x123456789abcdef0ULL);
  source.set_accuracy(accuracy); source.set_raw_text_info(35, 71);
  ParamFlag flags; flags.flag_ = 0xa5; flags.extend_flag_ = 0x5a;
  source.set_param_flag(flags); source.set_result_flag(0xfedcba98U);
  DatumParameter value;
  ASSERT_EQ(OB_SUCCESS, value.alloc_datum_reserved_buff(source.get_meta(), source.get_precision(), allocator));
  ASSERT_EQ(OB_SUCCESS, import_legacy_parameter(source, value, &allocator));
  EXPECT_TRUE(value.is_null());
  EXPECT_EQ(source.get_meta(), value.get_type_meta());
  EXPECT_EQ(source.get_param_meta(), value.get_param_meta());
  EXPECT_EQ(source.get_accuracy(), value.get_accuracy());
  EXPECT_EQ(source.get_result_flag(), value.get_result_flag());
  EXPECT_EQ(flags.flag_, value.get_param_flag().flag_);
  EXPECT_EQ(flags.extend_flag_, value.get_param_flag().extend_flag_);
  EXPECT_EQ(35, value.get_raw_text_pos()); EXPECT_EQ(71, value.get_raw_text_len());
}

TEST(DatumParameterConstraints, numeric_payload_is_contiguous_and_not_a_legacy_object)
{
  ObArenaAllocator allocator; number::ObNumber number;
  ASSERT_EQ(OB_SUCCESS, number.from("123456789012345678901234567890.123456789", allocator));
  ObObjParam source; source.set_number(number); source.set_precision(65); source.set_scale(30);
  DatumParameter value;
  ASSERT_EQ(OB_SUCCESS, value.alloc_datum_reserved_buff(source.get_meta(), source.get_precision(), allocator));
  ASSERT_EQ(OB_SUCCESS, import_legacy_parameter(source, value, &allocator));
  EXPECT_EQ(ObNumberType, value.get_type()); EXPECT_EQ(65, value.get_precision());
  EXPECT_EQ(30, value.get_scale());
  EXPECT_EQ(sizeof(number.get_desc()) + number.get_length() * sizeof(uint32_t), value.datum_.len_);
  EXPECT_EQ(0, memcmp(value.datum_.get_number_digits(), number.get_digits(), number.get_length() * sizeof(uint32_t)));
}

TEST(DatumParameterConstraints, imports_legacy_array_into_native_datum_elements)
{
  ObArenaAllocator allocator;
  ObSqlArrayObj *old = ObSqlArrayObj::alloc(allocator, 2);
  ASSERT_NE(nullptr, old);
  old->element_.set_obj_type(ObIntType);
  old->data_[0].set_int(123);
  old->data_[1].set_varchar("bytes");
  ObObjParam source;
  source.set_extend(reinterpret_cast<int64_t>(old), T_EXT_SQL_ARRAY);
  DatumParameter value;
  ASSERT_EQ(OB_SUCCESS, value.alloc_datum_reserved_buff(source.get_meta(), source.get_precision(), allocator));
  ASSERT_EQ(OB_SUCCESS, import_legacy_parameter(source, value, &allocator));
  const ObSqlDatumArray *array = value.get_array<ObSqlDatumArray>();
  ASSERT_NE(nullptr, array); ASSERT_EQ(2, array->count_);
  EXPECT_EQ(123, array->data_[0].get_int());
  EXPECT_EQ(ObString("bytes"), array->data_[1].get_string());
}

TEST(DatumParameterConstraints, invalid_constraint_reports_an_error)
{
  DatumParameter value; value.datum_.set_null(); bool matches = false;
  EXPECT_EQ(OB_ERR_UNEXPECTED, match(value, PRE_CALC_RESULT_NONE, matches));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
