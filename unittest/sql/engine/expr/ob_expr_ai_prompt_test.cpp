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
#include "lib/allocator/page_arena.h"
#include "common/object/ob_object.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace unittest
{

using namespace common;
using namespace sql;

class ObExprAIPromptTest : public ::testing::Test
{
public:
  ObExprAIPromptTest() : allocator_(), prompt_object_(NULL), args_array_(NULL) {}
  virtual ~ObExprAIPromptTest() {}

  virtual void SetUp() override
  {
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args_array_));
  }

  int add_string_arg(const char *str)
  {
    int ret = OB_SUCCESS;
    ObJsonString *json_str = NULL;
    ObString arg(str);
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator_, arg, json_str))) {
    } else if (OB_FAIL(args_array_->append(json_str))) {
    }
    return ret;
  }

  int add_json_arg(const char *json_text)
  {
    int ret = OB_SUCCESS;
    ObString text(json_text);
    ObIJsonBase *json_base = NULL;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, text,
                                                 ObJsonInType::JSON_TREE,
                                                 ObJsonInType::JSON_TREE, json_base))) {
    } else if (OB_FAIL(args_array_->append(static_cast<ObJsonNode *>(json_base)))) {
    }
    return ret;
  }

  int build_prompt(const char *template_str)
  {
    int ret = OB_SUCCESS;
    ObString tpl(template_str);
    if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(
                  allocator_, tpl, args_array_, prompt_object_))) {
    }
    return ret;
  }

  std::string render()
  {
    std::string result;
    ObString rendered;
    if (OB_SUCC(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                  allocator_, prompt_object_, rendered))) {
      result.assign(rendered.ptr(), rendered.length());
    }
    return result;
  }

  ObArenaAllocator allocator_;
  ObJsonObject *prompt_object_;
  ObJsonArray *args_array_;
};

TEST_F(ObExprAIPromptTest, repeated_placeholder)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg("ab"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}-{0}-{0}"));
  EXPECT_EQ("ab-ab-ab", render());
}

TEST_F(ObExprAIPromptTest, literal_brace_kept)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg("x"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("json:{a:1}"));
  EXPECT_EQ("json:{a:1}", render());
}

TEST_F(ObExprAIPromptTest, unicode_template_and_arg)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg("涓栫晫"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("浣犲ソ锛寋0}锛?));
  EXPECT_EQ("浣犲ソ锛屼笘鐣岋紒", render());
}

TEST_F(ObExprAIPromptTest, empty_string_arg)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg(""));
  ASSERT_EQ(OB_SUCCESS, build_prompt("a{0}b"));
  EXPECT_EQ("ab", render());
}

TEST_F(ObExprAIPromptTest, json_object_arg)
{
  ASSERT_EQ(OB_SUCCESS, add_json_arg("{\"a\":1}"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("data={0}"));
  std::string out = render();
  EXPECT_NE(std::string::npos, out.find("data={"));
  EXPECT_NE(std::string::npos, out.find("\"a\""));
  EXPECT_NE(std::string::npos, out.find("1"));
}

TEST_F(ObExprAIPromptTest, json_array_arg)
{
  ASSERT_EQ(OB_SUCCESS, add_json_arg("[1,2,3]"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("arr={0}"));
  std::string out = render();
  EXPECT_NE(std::string::npos, out.find("arr=["));
  EXPECT_NE(std::string::npos, out.find("3"));
}

TEST_F(ObExprAIPromptTest, nested_prompt_object)
{
  ASSERT_EQ(OB_SUCCESS, add_json_arg("{\"template\":\"inner {0}\",\"args\":[\"p\"]}"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("outer:{0}"));
  std::string out = render();
  EXPECT_NE(std::string::npos, out.find("outer:{"));
  EXPECT_NE(std::string::npos, out.find("template"));
}

TEST_F(ObExprAIPromptTest, invalid_placeholder_index)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg("x"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{5}"));
  ObString rendered;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
              allocator_, prompt_object_, rendered));
}

TEST_F(ObExprAIPromptTest, prompt_object_validity)
{
  ASSERT_EQ(OB_SUCCESS, add_string_arg("x"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}"));
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object_));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(NULL));
}

} // namespace unittest
} // namespace oceanbase
