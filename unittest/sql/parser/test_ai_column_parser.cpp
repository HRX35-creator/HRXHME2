/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_malloc.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

class AiColumnParserTest : public testing::Test
{
protected:
  int parse(const char *sql)
  {
    ObParser parser(allocator_, SMO_DEFAULT);
    return parser.parse(ObString::make_string(sql), result_);
  }
  ParseNode *find(ParseNode *node, ObItemType type)
  {
    if (node == nullptr || node->type_ == type) { return node; }
    for (int64_t i = 0; i < node->num_child_; ++i) {
      ParseNode *found = find(node->children_[i], type);
      if (found != nullptr) { return found; }
    }
    return nullptr;
  }
  ObArenaAllocator allocator_;
  ParseResult result_ = {};
};

TEST_F(AiColumnParserTest, create_preserves_expression_and_options)
{
  ASSERT_EQ(OB_SUCCESS, parse("CREATE TABLE docs(id BIGINT PRIMARY KEY, body TEXT, "
      "tags JSON AI COLUMN (AI_COMPLETE('model', CONCAT('分类: ', body))) "
      "WITH(sync_mode='async',max_retries=2))"));
  ParseNode *options = find(result_.result_tree_, T_AI_COLUMN_OPTIONS);
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(2, options->num_child_);
  ParseNode *option = options->children_[0];
  EXPECT_EQ(T_AI_COLUMN_OPTION, option->type_);
  EXPECT_EQ(ObString::make_string("sync_mode"), ObString(option->children_[0]->str_len_, option->children_[0]->str_value_));
}

TEST_F(AiColumnParserTest, add_and_modify_are_native_alter_actions)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs ADD COLUMN emb VECTOR(4) AI COLUMN "
      "(AI_EMBED('model', body, 4)) WITH(sync_mode='async')"));
  ASSERT_NE(nullptr, find(result_.result_tree_, T_COLUMN_ADD));
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs MODIFY COLUMN tags JSON AI COLUMN "
      "(AI_COMPLETE('model', body)) WITH(max_retries=0)"));
  ASSERT_NE(nullptr, find(result_.result_tree_, T_COLUMN_MODIFY));
}

TEST_F(AiColumnParserTest, retry_retains_where_clause)
{
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN tags "
      "WHERE status='FAILED' AND `status`='active' AND id IN (1,3)"));
  ParseNode *retry = find(result_.result_tree_, T_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, retry);
  ASSERT_EQ(2, retry->num_child_);
  ASSERT_NE(nullptr, retry->children_[1]);
  EXPECT_EQ(T_WHERE_CLAUSE, retry->children_[1]->type_);
  EXPECT_GT(retry->children_[1]->str_len_, 0);
  ASSERT_EQ(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN tags"));
  retry = find(result_.result_tree_, T_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, retry);
  EXPECT_EQ(nullptr, retry->children_[1]);
}

TEST_F(AiColumnParserTest, show_definition_and_ordinary_sql)
{
  ASSERT_EQ(OB_SUCCESS, parse("SHOW AI COLUMNS FROM test.docs"));
  ParseNode *show = find(result_.result_tree_, T_SHOW_COLUMNS);
  ASSERT_NE(nullptr, show);
  EXPECT_EQ(4, show->children_[0]->value_);
  ASSERT_EQ(OB_SUCCESS, parse("CREATE TABLE normal(id INT PRIMARY KEY, body TEXT, v INT AS(id+1))"));
  ASSERT_EQ(OB_SUCCESS, parse("SELECT retry, ai FROM normal WHERE id > 0"));
}

TEST_F(AiColumnParserTest, rejects_incomplete_syntax)
{
  EXPECT_NE(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN"));
  EXPECT_NE(OB_SUCCESS, parse("ALTER TABLE docs RETRY AI COLUMN tags WHERE"));
  EXPECT_NE(OB_SUCCESS, parse("CREATE TABLE docs(v JSON AI COLUMN ())"));
}
} // namespace sql
} // namespace oceanbase
