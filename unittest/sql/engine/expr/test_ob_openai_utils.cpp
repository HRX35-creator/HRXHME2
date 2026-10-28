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

// The unit tests of the openai and the ollama providers. They cover the
// validation of the embeddings, the recovery of the order of the embeddings
// that the model answers out of order and the native protocol of ollama.

#include <cstring>
#include <gtest/gtest.h>
#include "common/json_type/ob_json_tree.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{

class ObOpenAIUtilsTest : public ::testing::Test
{
public:
  ObOpenAIUtilsTest() : allocator_() {}
protected:
  ObArenaAllocator allocator_;

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

  // build a json array of the given numbers, it is used as an embedding
  ObJsonArray *build_number_array(const double *values, int64_t count)
  {
    ObJsonArray *array = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator_, array))) {
      return nullptr;
    }
    for (int64_t i = 0; i < count; i++) {
      ObJsonDouble *node = OB_NEWx(ObJsonDouble, &allocator_, values[i]);
      if (OB_ISNULL(node) || OB_FAIL(array->append(node))) {
        return nullptr;
      }
    }
    return array;
  }

  // build one element of the data array of an openai embedding answer
  ObJsonObject *build_embedding_item(ObJsonArray *embedding, int64_t index, bool with_index)
  {
    ObJsonObject *item = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator_, item))
        || OB_FAIL(item->add("embedding", embedding))) {
      return nullptr;
    }
    if (with_index && OB_FAIL(item->add("index", build_int(index)))) {
      return nullptr;
    }
    return item;
  }

  static double get_number(ObJsonNode *node, int64_t index)
  {
    double value = 0.0;
    if (OB_NOTNULL(node) && node->json_type() == ObJsonNodeType::J_ARRAY) {
      ObJsonNode *element = static_cast<ObJsonArray *>(node)->get_value(index);
      if (OB_NOTNULL(element) && element->json_type() == ObJsonNodeType::J_DOUBLE) {
        value = static_cast<ObJsonDouble *>(element)->value();
      }
    }
    return value;
  }
};

TEST_F(ObOpenAIUtilsTest, extract_embeddings_in_order)
{
  const double first[] = {0.1, 0.2};
  const double second[] = {0.3, 0.4};
  ObJsonArray *data_array = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, data_array));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(first, 2), 0, true)));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(second, 2), 1, true)));

  ObJsonArray *embeddings = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::extract_embeddings(allocator_, data_array, embeddings));
  ASSERT_NE(nullptr, embeddings);
  ASSERT_EQ(2, embeddings->element_count());
  ASSERT_DOUBLE_EQ(0.1, get_number(embeddings->get_value(0), 0));
  ASSERT_DOUBLE_EQ(0.3, get_number(embeddings->get_value(1), 0));
}

TEST_F(ObOpenAIUtilsTest, extract_embeddings_out_of_order)
{
  // the model may answer with the results in another order, the index of
  // every result is used to restore the order of the inputs
  const double first[] = {0.1, 0.2};
  const double second[] = {0.3, 0.4};
  ObJsonArray *data_array = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, data_array));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(second, 2), 1, true)));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(first, 2), 0, true)));

  ObJsonArray *embeddings = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::extract_embeddings(allocator_, data_array, embeddings));
  ASSERT_NE(nullptr, embeddings);
  ASSERT_EQ(2, embeddings->element_count());
  ASSERT_DOUBLE_EQ(0.1, get_number(embeddings->get_value(0), 0));
  ASSERT_DOUBLE_EQ(0.3, get_number(embeddings->get_value(1), 0));
}

TEST_F(ObOpenAIUtilsTest, extract_embeddings_without_index)
{
  // the answer without the index keeps the order of the answer
  const double first[] = {0.1};
  const double second[] = {0.2};
  ObJsonArray *data_array = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, data_array));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(first, 1), 0, false)));
  ASSERT_EQ(OB_SUCCESS, data_array->append(build_embedding_item(
                           build_number_array(second, 1), 1, false)));

  ObJsonArray *embeddings = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::extract_embeddings(allocator_, data_array, embeddings));
  ASSERT_NE(nullptr, embeddings);
  ASSERT_EQ(2, embeddings->element_count());
  ASSERT_DOUBLE_EQ(0.1, get_number(embeddings->get_value(0), 0));
  ASSERT_DOUBLE_EQ(0.2, get_number(embeddings->get_value(1), 0));
}

TEST_F(ObOpenAIUtilsTest, extract_embeddings_invalid_answer)
{
  const double first[] = {0.1};
  // the index is out of range
  ObJsonArray *out_of_range = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, out_of_range));
  ASSERT_EQ(OB_SUCCESS, out_of_range->append(build_embedding_item(
                           build_number_array(first, 1), 5, true)));
  ObJsonArray *embeddings = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::extract_embeddings(
                              allocator_, out_of_range, embeddings));

  // the index is duplicated
  ObJsonArray *duplicated = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, duplicated));
  ASSERT_EQ(OB_SUCCESS, duplicated->append(build_embedding_item(
                          build_number_array(first, 1), 0, true)));
  ASSERT_EQ(OB_SUCCESS, duplicated->append(build_embedding_item(
                          build_number_array(first, 1), 0, true)));
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::extract_embeddings(
                              allocator_, duplicated, embeddings));

  // the embedding is missing
  ObJsonArray *no_embedding = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, no_embedding));
  ObJsonObject *item = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, item));
  ASSERT_EQ(OB_SUCCESS, item->add("index", build_int(0)));
  ASSERT_EQ(OB_SUCCESS, no_embedding->append(item));
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::extract_embeddings(
                              allocator_, no_embedding, embeddings));
}

TEST_F(ObOpenAIUtilsTest, check_embedding_vector)
{
  const double values[] = {0.1, 0.2, 0.3};
  ObJsonArray *vector = build_number_array(values, 3);
  ASSERT_NE(nullptr, vector);

  // the dimension is not given, the length of the vector is not checked
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(vector, 0));
  // the dimension is given and it is the length of the vector
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(vector, 3));
  // the dimension is given and it is not the length of the vector
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(vector, 4));

  // the vector is not an array
  ObJsonString *not_an_array = build_string("not a vector");
  ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(not_an_array, 0));

  // the vector holds an element that is not a number
  ObJsonArray *invalid_vector = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, invalid_vector));
  ASSERT_EQ(OB_SUCCESS, invalid_vector->append(build_string("oops")));
  ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_vector(invalid_vector, 0));
}

TEST_F(ObOpenAIUtilsTest, ollama_complete_body)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObString model("llama3.1");
  ObString prompt("you are a helpful assistant");
  ObString content("what is the capital of France?");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, prompt, content, nullptr, body));
  ASSERT_NE(nullptr, body);

  ObJsonNode *model_node = body->get_value("model");
  ASSERT_NE(nullptr, model_node);
  ASSERT_EQ(ObJsonNodeType::J_STRING, model_node->json_type());

  ObJsonNode *prompt_node = body->get_value("prompt");
  ASSERT_NE(nullptr, prompt_node);
  ASSERT_EQ(ObJsonNodeType::J_STRING, prompt_node->json_type());

  // the answer of ollama is streamed by default, the whole answer is needed
  ObJsonNode *stream_node = body->get_value("stream");
  ASSERT_NE(nullptr, stream_node);
  ASSERT_EQ(ObJsonNodeType::J_BOOLEAN, stream_node->json_type());
  ASSERT_FALSE(static_cast<ObJsonBoolean *>(stream_node)->value());
}

TEST_F(ObOpenAIUtilsTest, ollama_complete_output)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObJsonObject *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, response));
  ASSERT_EQ(OB_SUCCESS, response->add("response", build_string("Paris")));

  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_STRING, result->json_type());

  // the answer of the model is empty
  ObJsonObject *empty_response = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, empty_response));
  ASSERT_EQ(OB_SUCCESS, empty_response->add("response", build_string("")));
  ASSERT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, empty_response, result));
}

TEST_F(ObOpenAIUtilsTest, ollama_embed_body)
{
  ObOllamaUtils::ObOllamaEmbed provider;
  ObString model("nomic-embed-text");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("first")));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("second")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, contents, nullptr, body));
  ASSERT_NE(nullptr, body);

  ObJsonNode *model_node = body->get_value("model");
  ASSERT_NE(nullptr, model_node);
  ASSERT_EQ(ObJsonNodeType::J_STRING, model_node->json_type());

  ObJsonNode *input_node = body->get_value("input");
  ASSERT_NE(nullptr, input_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, input_node->json_type());
  ASSERT_EQ(2, static_cast<ObJsonArray *>(input_node)->element_count());
}

TEST_F(ObOpenAIUtilsTest, ollama_embed_output)
{
  ObOllamaUtils::ObOllamaEmbed provider;
  const double values[] = {0.1, 0.2};
  ObJsonArray *embeddings = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, embeddings));
  ASSERT_EQ(OB_SUCCESS, embeddings->append(build_number_array(values, 2)));

  ObJsonObject *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, response));
  ASSERT_EQ(OB_SUCCESS, response->add("embeddings", embeddings));

  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ASSERT_EQ(1, static_cast<ObJsonArray *>(result)->element_count());

  // the old protocol of ollama answers with a single embedding
  ObJsonObject *old_response = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, old_response));
  ASSERT_EQ(OB_SUCCESS, old_response->add("embedding", build_number_array(values, 2)));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, old_response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ASSERT_EQ(1, static_cast<ObJsonArray *>(result)->element_count());
}

} // namespace common
} // namespace oceanbase
