/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{
TEST(AiColumnSchemaTest, definition_copy_owns_all_serialized_strings)
{
  ObColumnSchemaV2 copy;
  {
    ObColumnSchemaV2 original;
    AiColumnDefinition definition;
    definition.expression_ = ObString::make_string("AI_COMPLETE('m', body)");
    definition.normalized_expression_ = definition.expression_;
    definition.type_prompt_ = ObString::make_string("Return only text.");
    ASSERT_EQ(OB_SUCCESS, definition.dependency_ids_.push_back(17));
    ASSERT_EQ(OB_SUCCESS, definition.dependency_names_.push_back(ObString::make_string("body")));
    definition.state_column_id_ = 20;
    definition.request_column_id_ = 21;
    definition.result_spec_column_id_ = 22;
    definition.max_retries_ = 3;
    ASSERT_EQ(OB_SUCCESS, original.set_ai_column_definition(definition));
    ASSERT_EQ(OB_SUCCESS, copy.assign(original));
    EXPECT_NE(original.get_ai_column_info().ptr(), copy.get_ai_column_info().ptr());
  }
  AiColumnDefinition decoded;
  ASSERT_TRUE(copy.is_ai_column());
  ASSERT_EQ(OB_SUCCESS, copy.get_ai_column_definition(decoded));
  EXPECT_EQ(ObString::make_string("AI_COMPLETE('m', body)"), decoded.expression_);
  EXPECT_EQ(17, decoded.dependency_ids_.at(0));
  EXPECT_EQ(ObString::make_string("body"), decoded.dependency_names_.at(0));
  EXPECT_EQ(3, decoded.max_retries_);
  EXPECT_EQ(22, decoded.result_spec_column_id_);
}

TEST(AiColumnSchemaTest, rejects_truncated_or_invalid_definitions)
{
  ObColumnSchemaV2 column;
  AiColumnDefinition definition;
  EXPECT_EQ(OB_INVALID_ARGUMENT, column.set_ai_column_definition(definition));
  definition.expression_ = ObString::make_string("AI_COMPLETE('m',body)");
  definition.max_retries_ = -1;
  EXPECT_EQ(OB_INVALID_ARGUMENT, column.set_ai_column_definition(definition));
  definition.max_retries_ = 0;
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_definition(definition));
  ObString encoded = column.get_ai_column_info();
  ObColumnSchemaV2 other;
  EXPECT_NE(OB_SUCCESS, other.set_ai_column_info(ObString(encoded.length() - 1, encoded.ptr())));
  EXPECT_FALSE(other.is_ai_column());
  ASSERT_EQ(OB_SUCCESS, other.set_ai_column_info(encoded));
  ASSERT_EQ(OB_SUCCESS, other.set_ai_column_info(ObString()));
  EXPECT_FALSE(other.is_ai_column());
}

TEST(AiColumnSchemaTest, remapping_is_complete_and_failure_is_atomic)
{
  AiColumnDefinition definition;
  definition.state_column_id_ = 20;
  definition.request_column_id_ = 21;
  definition.result_spec_column_id_ = 22;
  ASSERT_EQ(OB_SUCCESS, definition.dependency_ids_.push_back(17));
  hash::ObHashMap<uint64_t, uint64_t> ids;
  ASSERT_EQ(OB_SUCCESS, ids.create(8, "AiColumnTest"));
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(17, 117));
  EXPECT_NE(OB_SUCCESS, definition.remap_column_ids(ids));
  EXPECT_EQ(17, definition.dependency_ids_.at(0));
  EXPECT_EQ(20, definition.state_column_id_);
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(20, 120));
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(21, 121));
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(22, 122));
  ASSERT_EQ(OB_SUCCESS, definition.remap_column_ids(ids));
  EXPECT_EQ(117, definition.dependency_ids_.at(0));
  EXPECT_EQ(120, definition.state_column_id_);
  EXPECT_EQ(122, definition.result_spec_column_id_);
}

TEST(AiColumnSchemaTest, execution_scopes_restore_nested_prompts)
{
  EXPECT_FALSE(AiColumnExecutionGuard::active());
  const ObString prompt = ObString::make_string("Return one JSON value.");
  {
    AiColumnExecutionGuard outer(&prompt);
    EXPECT_TRUE(AiColumnExecutionGuard::active());
    EXPECT_EQ(prompt, AiColumnExecutionGuard::type_prompt());
    {
      AiColumnExecutionGuard inner;
      EXPECT_EQ(prompt, AiColumnExecutionGuard::type_prompt());
    }
    EXPECT_TRUE(AiColumnExecutionGuard::active());
  }
  EXPECT_FALSE(AiColumnExecutionGuard::active());
  EXPECT_TRUE(AiColumnExecutionGuard::type_prompt().empty());
}
} // namespace schema
} // namespace share
} // namespace oceanbase
