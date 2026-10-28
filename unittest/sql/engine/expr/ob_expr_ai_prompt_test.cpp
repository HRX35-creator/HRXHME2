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

// The unit tests of the prompt object of ai_prompt. They cover the json
// arguments, the repeated placeholders, the nested prompt objects and the
// invalid index of a placeholder.

#include <cstring>
#include <gtest/gtest.h>
#include "common/json_type/ob_json_tree.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{

class ObExprAIPromptTest : public ::testing::Test
{
public:
  ObExprAIPromptTest() : allocator_() {}
protected:
  ObArenaAllocator allocator_;

  static bool is_same_str(const ObString &str, const char *expected)
  {
    return str.length() == static_cast<int32_t>(strlen(expected))
           && 0 == memcmp(str.ptr(), expected, str.length());
  }

  ObJsonString *build_string(const char *str)
  {
    ObJsonString *node = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator_, ObString(str), node))) {
      return nullptr;
    }
    return node;
  }

  ObJsonInt *build_int(int64_t value)
  {
    ObJsonInt *node = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator_, value, node))) {
      return nullptr;
    }
    return node;
  }

  // build the args array of a prompt object, every argument is a string
  ObJsonArray *build_string_args(const char *const *args, int64_t count)
  {
    ObJsonArray *args_array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator_, args_array))) {
      return nullptr;
    }
    for (int64_t i = 0; i < count; i++) {
      ObJsonString *arg = build_string(args[i]);
      if (OB_ISNULL(arg) || OB_FAIL(args_array->append(arg))) {
        return nullptr;
      }
    }
    return args_array;
  }

  // build the prompt object {"template": templ, "args": args}
  ObJsonObject *build_prompt(const char *templ, ObJsonArray *args)
  {
    ObJsonObject *prompt = nullptr;
    ObJsonString *templ_node = build_string(templ);
    if (OB_ISNULL(templ_node)
        || OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator_, prompt))
        || OB_FAIL(prompt->add(ObAIFuncPromptObjectUtils::prompt_template_key, templ_node))
        || OB_FAIL(prompt->add(ObAIFuncPromptObjectUtils::prompt_args_key, args))) {
      return nullptr;
    }
    return prompt;
  }
};

TEST_F(ObExprAIPromptTest, replace_placeholders)
{
  const char *args[] = {"one", "two"};
  ObJsonArray *args_array = build_string_args(args, 2);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("{0} and {1}", args_array);
  ASSERT_NE(nullptr, prompt);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_TRUE(is_same_str(replaced, "one and two"));
}

TEST_F(ObExprAIPromptTest, replace_repeated_placeholders)
{
  // the same placeholder may be used more than once, every use is replaced
  const char *args[] = {"x"};
  ObJsonArray *args_array = build_string_args(args, 1);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("{0}-{0}-{0}", args_array);
  ASSERT_NE(nullptr, prompt);

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_TRUE(is_same_str(replaced, "x-x-x"));
}

TEST_F(ObExprAIPromptTest, replace_repeated_placeholder_with_long_value)
{
  // the result must be long enough to hold every replacement
  const char *args[] = {"0123456789"};
  ObJsonArray *args_array = build_string_args(args, 1);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("[{0}][{0}][{0}]", args_array);
  ASSERT_NE(nullptr, prompt);

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_TRUE(is_same_str(replaced, "[0123456789][0123456789][0123456789]"));
}

TEST_F(ObExprAIPromptTest, replace_invalid_placeholder_index)
{
  const char *args[] = {"one"};
  ObJsonArray *args_array = build_string_args(args, 1);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("{1}", args_array);
  ASSERT_NE(nullptr, prompt);

  ObString replaced;
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                                 allocator_, prompt, replaced));
}

TEST_F(ObExprAIPromptTest, replace_unicode_and_escape)
{
  // the unicode and the escaped characters are kept as they are
  const char *args[] = {"\xe4\xb8\x96\xe7\x95\x8c", "a\"b"};
  ObJsonArray *args_array = build_string_args(args, 2);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("{0}-{1}", args_array);
  ASSERT_NE(nullptr, prompt);

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_TRUE(is_same_str(replaced, "\xe4\xb8\x96\xe7\x95\x8c-a\"b"));
}

TEST_F(ObExprAIPromptTest, replace_json_argument)
{
  // an argument that is not a string is replaced by its json text
  ObJsonArray *args_array = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args_array));
  ASSERT_NE(nullptr, args_array);
  ASSERT_EQ(OB_SUCCESS, args_array->append(build_int(42)));

  ObJsonObject *prompt = build_prompt("value:{0}", args_array);
  ASSERT_NE(nullptr, prompt);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_TRUE(is_same_str(replaced, "value:42"));
}

TEST_F(ObExprAIPromptTest, replace_nested_prompt)
{
  // a nested prompt object is rendered with its own arguments
  const char *inner_args[] = {"inner"};
  ObJsonArray *inner_args_array = build_string_args(inner_args, 1);
  ASSERT_NE(nullptr, inner_args_array);
  ObJsonObject *inner_prompt = build_prompt("[{0}]", inner_args_array);
  ASSERT_NE(nullptr, inner_prompt);

  ObJsonArray *args_array = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args_array));
  ASSERT_NE(nullptr, args_array);
  ASSERT_EQ(OB_SUCCESS, args_array->append(inner_prompt));

  ObJsonObject *prompt = build_prompt("outer:{0}", args_array);
  ASSERT_NE(nullptr, prompt);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));

  // the nested prompt object is rendered with its own arguments first
  ObString inner_replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, inner_prompt, inner_replaced));
  ASSERT_TRUE(is_same_str(inner_replaced, "[inner]"));

  // the nested prompt object is replaced by its json text
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                          allocator_, prompt, replaced));
  ASSERT_GT(replaced.length(), 0);
}

TEST_F(ObExprAIPromptTest, is_valid_prompt_object)
{
  const char *args[] = {"one"};
  ObJsonArray *args_array = build_string_args(args, 1);
  ASSERT_NE(nullptr, args_array);

  ObJsonObject *prompt = build_prompt("{0}", args_array);
  ASSERT_NE(nullptr, prompt);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));

  // the template is missing
  ObJsonObject *no_template = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, no_template));
  ASSERT_EQ(OB_SUCCESS, no_template->add(ObAIFuncPromptObjectUtils::prompt_args_key, args_array));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(no_template));

  // the args are missing
  ObJsonObject *no_args = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, no_args));
  ASSERT_EQ(OB_SUCCESS, no_args->add(ObAIFuncPromptObjectUtils::prompt_template_key,
                                      build_string("{0}")));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(no_args));

  // the json null is not a valid argument
  ObJsonArray *null_args = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, null_args));
  ASSERT_NE(nullptr, null_args);
  ASSERT_EQ(OB_SUCCESS, null_args->append(OB_NEWx(ObJsonNull, &allocator_)));
  ObJsonObject *null_prompt = build_prompt("{0}", null_args);
  ASSERT_NE(nullptr, null_prompt);
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(null_prompt));
}

TEST_F(ObExprAIPromptTest, is_valid_prompt_arg)
{
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_arg(build_string("str")));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_arg(build_int(1)));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_arg(
      OB_NEWx(ObJsonNull, &allocator_)));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_arg(nullptr));
}

TEST_F(ObExprAIPromptTest, construct_prompt_object)
{
  const char *args[] = {"one"};
  ObJsonArray *args_array = build_string_args(args, 1);
  ASSERT_NE(nullptr, args_array);

  ObString templ("hello {0}");
  ObJsonObject *prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                          allocator_, templ, args_array, prompt));
  ASSERT_NE(nullptr, prompt);
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));

  ObJsonNode *templ_node = prompt->get_value(ObAIFuncPromptObjectUtils::prompt_template_key);
  ASSERT_NE(nullptr, templ_node);
  ASSERT_EQ(ObJsonNodeType::J_STRING, templ_node->json_type());
  ASSERT_TRUE(is_same_str(static_cast<ObJsonString *>(templ_node)->get_str(), "hello {0}"));

  ObJsonNode *args_node = prompt->get_value(ObAIFuncPromptObjectUtils::prompt_args_key);
  ASSERT_NE(nullptr, args_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, args_node->json_type());
  ASSERT_EQ(1, static_cast<ObJsonArray *>(args_node)->element_count());
}

} // namespace common
} // namespace oceanbase
