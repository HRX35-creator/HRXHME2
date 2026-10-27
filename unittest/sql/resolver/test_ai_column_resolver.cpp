/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/resolver/ddl/ob_ddl_resolver.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

namespace
{
int add_test_column(ObTableSchema &table, uint64_t id, const char *name,
                    const AiColumnDefinition *definition = nullptr)
{
  ObColumnSchemaV2 column;
  column.set_table_id(table.get_table_id());
  column.set_column_id(id);
  column.set_data_type(ObVarcharType);
  column.set_data_length(200);
  column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  int ret = column.set_column_name(name);
  if (ret == OB_SUCCESS && definition != nullptr) { ret = column.set_ai_column_definition(*definition); }
  if (ret == OB_SUCCESS) { ret = table.add_column(column); }
  return ret;
}

AiColumnDefinition make_definition(const char *dependency)
{
  AiColumnDefinition definition;
  definition.expression_ = ObString::make_string("AI_COMPLETE('model',body)");
  definition.normalized_expression_ = definition.expression_;
  (void)definition.dependency_names_.push_back(ObString::make_string(dependency));
  return definition;
}
}

TEST(AiColumnResolverTest, resolves_dependencies_and_creates_hidden_state_once)
{
  ObTableSchema table;
  table.set_table_id(50001);
  ASSERT_EQ(OB_SUCCESS, add_test_column(table, 16, "body"));
  AiColumnDefinition definition = make_definition("body");
  ASSERT_EQ(OB_SUCCESS, add_test_column(table, 17, "summary", &definition));
  uint64_t last_id = 17;
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::finalize_ai_columns(table, last_id));
  EXPECT_EQ(20, last_id);
  EXPECT_EQ(5, table.get_column_count());
  AiColumnDefinition resolved;
  ASSERT_EQ(OB_SUCCESS, table.get_column_schema(17)->get_ai_column_definition(resolved));
  ASSERT_EQ(1, resolved.dependency_ids_.count());
  EXPECT_EQ(16, resolved.dependency_ids_.at(0));
  const uint64_t hidden[] = {resolved.state_column_id_, resolved.request_column_id_, resolved.result_spec_column_id_};
  for (uint64_t id : hidden) {
    ASSERT_NE(nullptr, table.get_column_schema(id));
    EXPECT_TRUE(table.get_column_schema(id)->is_hidden());
    EXPECT_TRUE(table.get_column_schema(id)->is_nullable());
  }
  EXPECT_FALSE(table.get_column_schema(17)->is_hidden());
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::finalize_ai_columns(table, last_id));
  EXPECT_EQ(20, last_id);
  EXPECT_EQ(5, table.get_column_count());
}

TEST(AiColumnResolverTest, rejects_missing_and_ai_dependencies)
{
  ObTableSchema missing;
  missing.set_table_id(50002);
  AiColumnDefinition definition = make_definition("unknown_column");
  ASSERT_EQ(OB_SUCCESS, add_test_column(missing, 16, "summary", &definition));
  uint64_t last_id = 16;
  EXPECT_EQ(OB_ERR_BAD_FIELD_ERROR, ObDDLResolver::finalize_ai_columns(missing, last_id));
  EXPECT_EQ(1, missing.get_column_count());

  ObTableSchema chained;
  chained.set_table_id(50003);
  definition = make_definition("summary");
  ASSERT_EQ(OB_SUCCESS, add_test_column(chained, 16, "summary", &definition));
  last_id = 16;
  EXPECT_EQ(OB_NOT_SUPPORTED, ObDDLResolver::finalize_ai_columns(chained, last_id));
  EXPECT_EQ(1, chained.get_column_count());
}

TEST(AiColumnResolverTest, refuses_hidden_name_collision)
{
  ObTableSchema table;
  table.set_table_id(50004);
  ASSERT_EQ(OB_SUCCESS, add_test_column(table, 16, "body"));
  ASSERT_EQ(OB_SUCCESS, add_test_column(table, 17, "__ai_state_18"));
  AiColumnDefinition definition = make_definition("body");
  ASSERT_EQ(OB_SUCCESS, add_test_column(table, 18, "summary", &definition));
  uint64_t last_id = 18;
  EXPECT_EQ(OB_ERR_COLUMN_DUPLICATE, ObDDLResolver::finalize_ai_columns(table, last_id));
  EXPECT_EQ(3, table.get_column_count());
}
} // namespace sql
} // namespace oceanbase
