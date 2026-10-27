/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 */
#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
class AIResponseTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *parse(const char *json)
  {
    ObString text(json);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, result));
    return result;
  }
};

TEST_F(AIResponseTest, embedding_restores_response_indices)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *output = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"data":[{"index":1,"embedding":[20.0,21.0]},{"index":0,"embedding":[10.0,11.0]}]})"),
      output));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, output->json_type());
  auto *array = static_cast<ObJsonArray *>(output);
  ASSERT_EQ(2, array->element_count());
  auto *first = static_cast<ObJsonArray *>(array->get_value(0));
  EXPECT_DOUBLE_EQ(10.0, first->get_value(0)->get_double());
}

TEST_F(AIResponseTest, embedding_rejects_malformed_mapping_and_vectors)
{
  const char *cases[] = {
      R"({"data":{}})",
      R"({"data":[]})",
      R"({"data":[{"index":0,"embedding":[1.0]},{"index":0,"embedding":[2.0]}]})",
      R"({"data":[{"index":-1,"embedding":[1.0]}]})",
      R"({"data":[{"index":1,"embedding":[1.0]}]})",
      R"({"data":[{"index":"0","embedding":[1.0]}]})",
      R"({"data":[{"embedding":[1.0]}]})",
      R"({"data":[{"index":0,"embedding":"not-a-vector"}]})",
      R"({"data":[{"index":0,"embedding":[]} ]})",
      R"({"data":[{"index":0,"embedding":[true]}]})",
      R"({"data":[{"index":0,"embedding":[1e100]}]})",
  };
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *json : cases) {
    ObIJsonBase *output = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, parse(json), output)) << json;
  }
}

TEST_F(AIResponseTest, completion_rejects_empty_or_structured_content)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (const char *json : {
       R"({"choices":[{"message":{"content":""}}]})",
       R"({"choices":[{"message":{"content":null}}]})",
       R"({"choices":[{"message":{"content":{}}}]})",
       R"({"choices":[]})"}) {
    ObIJsonBase *output = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, parse(json), output));
  }
  ObIJsonBase *output = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"choices":[{"message":{"content":"valid"}}]})"), output));
}

TEST_F(AIResponseTest, ollama_generation_forces_non_streaming)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObArray<ObString> headers;
  ObString no_api_key;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator_, no_api_key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(ObString::make_string("Content-Type: application/json"), headers.at(0));
  ObString model("local-model"), system(""), prompt("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, system, prompt,
      parse(R"({"stream":true,"temperature":0.5})"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ObIJsonBase *output = nullptr;
  EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, parse(R"({"response":null})"), output));
}

TEST_F(AIResponseTest, rerank_has_deterministic_ties_and_accepts_integer_scores)
{
  auto *root = parse(R"({"results":[{"index":2,"relevance_score":1},{"index":0,"relevance_score":1.0},{"index":1,"relevance_score":2.0}]})");
  auto *results = static_cast<ObJsonArray *>(root->get_value("results"));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_rerank_results(allocator_, 3, results));
  const int64_t expected[] = {1, 0, 2};
  for (int64_t i = 0; i < 3; ++i) {
    auto *item = static_cast<ObJsonObject *>(results->get_value(i));
    EXPECT_EQ(expected[i], item->get_value("index")->get_int());
    EXPECT_EQ(ObJsonNodeType::J_DOUBLE, item->get_value("relevance_score")->json_type());
  }
}

TEST_F(AIResponseTest, rerank_rejects_duplicate_missing_or_invalid_results)
{
  for (const char *json : {
       R"({"results":[{"index":0,"relevance_score":1.0},{"index":0,"relevance_score":0.5}]})",
       R"({"results":[{"index":0,"relevance_score":1.0}]})",
       R"({"results":[{"index":0,"relevance_score":"1"},{"index":1,"relevance_score":0.5}]})",
       R"({"results":[{"index":0,"relevance_score":1.0},{"index":9,"relevance_score":0.5}]})"}) {
    auto *root = parse(json);
    auto *results = static_cast<ObJsonArray *>(root->get_value("results"));
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank_results(allocator_, 2, results));
  }
}

TEST_F(AIResponseTest, rerank_provider_checks_container_types)
{
  ObDashscopeUtils::ObDashscopeRerank dashscope;
  ObSiliconflowUtils::ObSiliconflowRerank siliconflow;
  ObIJsonBase *output = nullptr;
  EXPECT_NE(OB_SUCCESS, dashscope.parse_output(allocator_, parse(R"({"output":[]})"), output));
  EXPECT_NE(OB_SUCCESS, siliconflow.parse_output(allocator_, parse(R"({"results":{}})"), output));
}
} // namespace common
} // namespace oceanbase
