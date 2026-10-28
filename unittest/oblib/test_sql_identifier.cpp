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

#include <gtest/gtest.h>
#include <string>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(SqlIdentifier, doubles_backticks)
{
  const char *inputs[] = {"plain", "a`b", "`", "``", "`a`", "列`名"};
  const char *expected[] = {"plain", "a``b", "``", "````", "``a``", "列``名"};
  for (int i = 0; i < 6; ++i) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(inputs[i])));
    EXPECT_STREQ(expected[i], sql.ptr());
  }
}

TEST(SqlIdentifier, respects_string_length)
{
  const char input[] = {'x', '`', 'y', 'z'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(3, input)));
  EXPECT_STREQ("x``y", sql.ptr());
}

TEST(SqlIdentifier, empty_input_preserves_output)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("SELECT "));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  EXPECT_STREQ("SELECT ", sql.ptr());
}

TEST(SqlIdentifier, retains_prefix_and_suffix)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("cast(`"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("e`num")));
  ASSERT_EQ(OB_SUCCESS, sql.append("` as unsigned) AS `__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("e`num")));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  EXPECT_STREQ("cast(`e``num` as unsigned) AS `__SHADOW_e``num`", sql.ptr());
}

TEST(SqlIdentifier, grows_output_for_many_quotes)
{
  const std::string input(1024, '`');
  const std::string expected(2048, '`');
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(input.size(), input.data())));
  EXPECT_EQ(expected, std::string(sql.ptr(), sql.length()));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
