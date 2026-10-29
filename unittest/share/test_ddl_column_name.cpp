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
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_ddl_common.h"

namespace oceanbase
{
namespace share
{
using namespace common;

// Exercise every formatting flag independently of the identifier contents.
static void check_column_name(const ObString &name, const char *quoted, const char *shadow_quoted)
{
  for (bool shadow : {false, true}) {
    for (bool cast : {false, true}) {
      for (bool origin : {false, true}) {
        for (bool alias : {false, true}) {
          for (bool comma : {false, true}) {
            ObSqlString sql;
            ObColumnNameInfo info(name, shadow, cast);
            ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(info, origin, alias, comma, sql));
            std::string expected = comma ? ", " : "";
            if (origin) {
              expected += cast ? std::string("cast(") + quoted + " as unsigned)" : quoted;
            }
            if (origin && alias) {
              expected += " AS ";
            }
            if (alias) {
              expected += shadow ? shadow_quoted : quoted;
            }
            EXPECT_EQ(expected, std::string(sql.empty() ? "" : sql.ptr(), sql.length()));
          }
        }
      }
    }
  }
}

TEST(TestDDLColumnName, OrdinaryIdentifiers)
{
  check_column_name(ObString::make_string("value"), "`value`", "`__SHADOW_value`");
  check_column_name(ObString::make_string("select"), "`select`", "`__SHADOW_select`");
  check_column_name(ObString::make_string("a b'\\c"), "`a b'\\c`", "`__SHADOW_a b'\\c`");
}

TEST(TestDDLColumnName, EmbeddedBackticks)
{
  check_column_name(ObString::make_string("key`id"), "`key``id`", "`__SHADOW_key``id`");
  check_column_name(ObString::make_string("`leading"), "```leading`", "`__SHADOW_``leading`");
  check_column_name(ObString::make_string("trailing`"), "`trailing```", "`__SHADOW_trailing```");
  check_column_name(ObString::make_string("a``b"), "`a````b`", "`__SHADOW_a````b`");
  check_column_name(ObString::make_string("`"), "````", "`__SHADOW_```");
}

TEST(TestDDLColumnName, LengthDelimitedIdentifier)
{
  char name[] = {'k', '`', 'y', 'x'};
  check_column_name(ObString(3, name), "`k``y`", "`__SHADOW_k``y`");
}

TEST(TestDDLColumnName, MultibyteIdentifier)
{
  check_column_name(ObString::make_string("\xE5\x88\x97`x"), "`\xE5\x88\x97``x`", "`__SHADOW_\xE5\x88\x97``x`");
}

TEST(TestDDLColumnName, MaximumLengthBacktickIdentifier)
{
  const std::string name(64, '`');
  const std::string quoted(130, '`');
  const std::string shadow_quoted = "`__SHADOW_" + std::string(129, '`');
  check_column_name(ObString(name.length(), name.data()), quoted.c_str(), shadow_quoted.c_str());
}

TEST(TestDDLColumnName, ColumnList)
{
  ObSEArray<ObColumnNameInfo, 4> names;
  ASSERT_EQ(OB_SUCCESS, names.push_back(ObColumnNameInfo(ObString::make_string("key`id"), false)));
  ASSERT_EQ(OB_SUCCESS, names.push_back(ObColumnNameInfo(ObString::make_string("enum`value"), true, true)));
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(names, true, true, false, sql));
  EXPECT_STREQ("`key``id` AS `key``id`, cast(`enum``value` as unsigned) AS `__SHADOW_enum``value`", sql.ptr());
}

TEST(TestDDLColumnName, HeapHiddenPrimaryKey)
{
  ObSEArray<ObColumnNameInfo, 4> names;
  const ObColumnNameInfo hidden(ObString::make_string(OB_HIDDEN_PK_INCREMENT_COLUMN_NAME), false);
  ASSERT_EQ(OB_SUCCESS, names.push_back(hidden));
  ASSERT_EQ(OB_SUCCESS, names.push_back(ObColumnNameInfo(ObString::make_string("first`col"), false)));
  ASSERT_EQ(OB_SUCCESS, names.push_back(hidden));
  ASSERT_EQ(OB_SUCCESS, names.push_back(ObColumnNameInfo(ObString::make_string("last`col"), false)));
  ASSERT_EQ(OB_SUCCESS, names.push_back(hidden));
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(names, true, false, true, sql));
  EXPECT_STREQ("`first``col`, `last``col`", sql.ptr());

  names.reset();
  ASSERT_EQ(OB_SUCCESS, names.push_back(hidden));
  sql.reuse();
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(names, true, false, true, sql));
  EXPECT_TRUE(sql.empty());
  ASSERT_EQ(OB_SUCCESS, ObDDLUtil::generate_column_name_str(names, true, false, false, sql));
  EXPECT_STREQ("`__pk_increment`", sql.ptr());
}

TEST(TestDDLColumnName, EmptyColumnList)
{
  ObSEArray<ObColumnNameInfo, 4> names;
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, sql.append("prefix"));
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObDDLUtil::generate_column_name_str(names, true, true, false, sql));
  EXPECT_STREQ("prefix", sql.ptr());
}
} // namespace share
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
