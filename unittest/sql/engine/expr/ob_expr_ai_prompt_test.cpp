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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
int render(ObArenaAllocator &allocator, const char *json, ObString &result)
{
  ObString input(json);
  ObJsonObject *object = nullptr;
  int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object);
  return ret == OB_SUCCESS
      ? ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, result) : ret;
}
}

TEST(AIPrompt, RepeatedPlaceholderAndLiteralReplacement)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{0}/{0}/{0}/{0}","args":["long replacement {1} \\ \" text"]})", result));
  EXPECT_EQ(ObString("long replacement {1} \\ \" text/long replacement {1} \\ \" text/long replacement {1} \\ \" text/long replacement {1} \\ \" text"), result);
}

TEST(AIPrompt, JsonAndNestedPrompt)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{0} {1} {2}","args":[[1,2],{"a":1},{"template":"hello {0}","args":["world"]}]})", result));
  // Ignore formatting spaces in JSON, but preserve the nested prompt as text.
  std::string text(result.ptr(), result.length());
  EXPECT_NE(std::string::npos, text.find("hello world"));
  EXPECT_NE(std::string::npos, text.find("[1"));
  EXPECT_NE(std::string::npos, text.find("\"a\""));
}

TEST(AIPrompt, RejectInvalidIndexWithoutOverflow)
{
  ObArenaAllocator allocator;
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator, R"({"template":"{1}","args":["x"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator,
      R"({"template":"{99999999999999999999999999999999}","args":["x"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator, R"({"template":"{-1}","args":["x"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(allocator, R"({"template":[],"args":[]})", result));
}

TEST(AIPrompt, LongUnicodeTemplate)
{
  ObArenaAllocator allocator;
  ObString result;
  std::string value(100000, 'x');
  std::string json = "{\"template\":\"浣犲ソ {0}{0}\",\"args\":[\"" + value + "\"]}";
  ASSERT_EQ(OB_SUCCESS, render(allocator, json.c_str(), result));
  EXPECT_EQ(200007, result.length());
}
