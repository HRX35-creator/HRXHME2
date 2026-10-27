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
#include <limits>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
ObJsonObject *parse(ObArenaAllocator &allocator, const char *text)
{
  ObString input(text);
  ObJsonObject *object = nullptr;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object));
  return object;
}
}

TEST(OpenAIUtils, RestoreEmbeddingOrder)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  auto *response = parse(allocator, R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})");
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(result, 2, 2));
  auto *first = static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(result)->get_value(0));
  EXPECT_EQ(1, first->get_value(0)->get_int());
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::validate_embeddings(result, 3, 2));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::validate_embeddings(result, 2, 3));
}

TEST(OpenAIUtils, RejectMalformedEmbeddings)
{
  const char *cases[] = {
    R"({"data":{}})", R"({"data":[]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[{"index":0,"embedding":[null]}]})",
    R"({"data":[{"index":0,"embedding":["1"]}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})"
  };
  for (const char *text : cases) {
    ObArenaAllocator allocator;
    ObOpenAIUtils::ObOpenAIEmbed provider;
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, text), result)) << text;
  }
}

TEST(OpenAIUtils, RejectNonfiniteEmbedding)
{
  ObArenaAllocator allocator;
  auto *object = parse(allocator, R"({"vectors":[[1]]})");
  auto *vectors = static_cast<ObJsonArray *>(object->get_value("vectors"));
  auto *vector = static_cast<ObJsonArray *>(vectors->get_value(0));
  auto *number = OB_NEWx(ObJsonDouble, &allocator, std::numeric_limits<double>::infinity());
  ASSERT_EQ(OB_SUCCESS, vector->append(number));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::validate_embeddings(vectors));
}

TEST(OpenAIUtils, CompletionRequiresNonemptyString)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObIJsonBase *result = nullptr;
  for (const char *text : {R"({})", R"({"choices":[]})",
       R"({"choices":[{"message":{"content":""}}]})",
       R"({"choices":[{"message":{"content":null}}]})"}) {
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, text), result));
  }
  EXPECT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      parse(allocator, R"({"choices":[{"message":{"content":"ok"}}]})"), result));
}

TEST(OllamaUtils, NativeProtocolAndCaseInsensitiveProvider)
{
  ObArenaAllocator allocator;
  ObAIFuncIComplete *complete = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("oLlAmA"), complete));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ObString("ollama"), embed));
  ObString key, system, model("test-model"), prompt("hello");
  ObArray<ObString> headers, inputs;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete->get_header(allocator, key, headers));
  ASSERT_EQ(1, headers.count());
  auto *config = parse(allocator, R"({"temperature":0.25,"max_tokens":16,"stream":true})");
  ASSERT_EQ(OB_SUCCESS, complete->get_body(allocator, model, system, prompt, config, body));
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options);
  EXPECT_DOUBLE_EQ(0.25, options->get_value("temperature")->get_double());
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("prompt"));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(prompt));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(prompt));
  ASSERT_EQ(OB_SUCCESS, embed->get_body(allocator, model, inputs, nullptr, body));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, embed->parse_output(allocator, parse(allocator, R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_NE(OB_SUCCESS, embed->parse_output(allocator, parse(allocator, R"({"embeddings":[[true]]})"), result));
  EXPECT_EQ(OB_SUCCESS, complete->parse_output(allocator, parse(allocator, R"({"response":"ok","done":true})"), result));
  EXPECT_NE(OB_SUCCESS, complete->parse_output(allocator, parse(allocator, R"({"response":""})"), result));
}

TEST(AIRerank, ValidateAndStableSort)
{
  ObArenaAllocator allocator;
  auto *response = parse(allocator, R"({"results":[{"index":2,"relevance_score":1},{"index":1,"relevance_score":2},{"index":0,"relevance_score":1}]})");
  ObJsonArray *sorted = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_rerank(allocator, response->get_value("results"), 3, sorted));
  const int order[] = {1, 0, 2};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(order[i], static_cast<ObJsonObject *>(sorted->get_value(i))->get_value("index")->get_int());
  }
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank(allocator, response->get_value("results"), 2, sorted));
  response = parse(allocator, R"({"results":[{"index":0,"relevance_score":1},{"index":0,"relevance_score":2}]})");
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_rerank(allocator, response->get_value("results"), 2, sorted));
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
