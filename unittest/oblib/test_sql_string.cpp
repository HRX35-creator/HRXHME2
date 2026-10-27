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

#include <string>
#include <gtest/gtest.h>
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

TEST(TestSqlString, escaped_identifier)
{
  struct TestCase {
    const char *name;
    const char *escaped;
  };
  const TestCase cases[] = {
    {"normal_name", "normal_name"},
    {"a`b", "a``b"},
    {"`leading", "``leading"},
    {"trailing`", "trailing``"},
    {"a``b", "a````b"},
    {"列`名", "列``名"},
    {"a\\b\"c", "a\\b\"c"},
  };
  for (const TestCase &test : cases) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string(test.name)));
    EXPECT_EQ(std::string(test.escaped), std::string(sql.ptr(), sql.length()));
  }
}

TEST(TestSqlString, escaped_identifier_respects_length)
{
  const char name[] = {'a', '`', 'b', 'x', 'y'};
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(3, name)));
  EXPECT_EQ("a``b", std::string(sql.ptr(), sql.length()));
}

TEST(TestSqlString, escaped_identifier_keeps_prefix_and_empty_input)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString()));
  EXPECT_EQ(0, sql.length());
  ASSERT_EQ(OB_SUCCESS, sql.append("`__SHADOW_"));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString::make_string("a`b")));
  ASSERT_EQ(OB_SUCCESS, sql.append("`"));
  EXPECT_EQ("`__SHADOW_a``b`", std::string(sql.ptr(), sql.length()));
}

TEST(TestSqlString, escaped_identifier_grows_output)
{
  const std::string name(2048, '`');
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(ObString(name.size(), name.data())));
  EXPECT_EQ(std::string(name.size() * 2, '`'), std::string(sql.ptr(), sql.length()));
}

TEST(TestSqlString, escaped_identifier_retains_aliased_input)
{
  std::string name(600, 'a');
  name.insert(300, "`");
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.assign(name.data(), name.size()));
  ASSERT_EQ(OB_SUCCESS, sql.append_escaped_identifier(sql.string()));
  std::string escaped = name;
  escaped.insert(300, "`");
  EXPECT_EQ(name + escaped, std::string(sql.ptr(), sql.length()));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
