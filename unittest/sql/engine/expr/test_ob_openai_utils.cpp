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

namespace oceanbase
{
namespace sql
{

class ObOpenAIUtilsTest : public ::testing::Test
{
public:
  ObOpenAIUtilsTest() {}
  virtual ~ObOpenAIUtilsTest() {}
  virtual void SetUp() {}
  virtual void TearDown() {}
};

static ObJsonObject *build_json_object(ObIAllocator &allocator, const char *json_str)
{
  ObString str(json_str);
  ObIJsonBase *j_base = nullptr;
  if (OB_SUCCESS != ObJsonBaseFactory::get_json_base(&allocator, str,
                                                     ObJsonInType::JSON_TREE,
                                                     ObJsonInType::JSON_TREE, j_base)) {
    return nullptr;
  } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_OBJECT) {
    return nullptr;
  }
  return static_cast<ObJsonObject *>(j_base);
}

static ObString print_node(ObIAllocator &allocator, ObIJsonBase *node)
{
  ObJsonBuffer json_buf(&allocator);
  ObString res;
  if (OB_NOT_NULL(node) && OB_SUCCESS == node->print(json_buf, false)) {
    res = json_buf.string();
  }
  return res;
}

TEST_F(ObOpenAIUtilsTest, test_openai_complete_get_header)
{
  ObArenaAllocator allocator;
  ObArray<ObString> headers;
  ObString api_key("sk-1234567890");
  ObOpenAIUtils::ObOpenAIComplete completion;
  ASSERT_EQ(OB_SUCCESS, completion.get_header(allocator, api_key, headers));
  ASSERT_EQ(2LL, headers.count());
  ASSERT_EQ(ObString("Authorization: Bearer sk-1234567890"), headers.at(0));
  ASSERT_EQ(ObString("Content-Type: application/json"), headers.at(1));

  // an empty api key cannot build an Authorization header
  ObString empty_key;
  ObArray<ObString> empty_headers;
  ASSERT_EQ(OB_INVALID_ARGUMENT, completion.get_header(allocator, empty_key, empty_headers));
}

TEST_F(ObOpenAIUtilsTest, test_openai_embed_get_body)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObString model("text-embedding-v1");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("first")));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("second")));
  ObJsonObject *body = nullptr;
  // one request carries every input of the batch
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator, model, contents, nullptr, body));
  ASSERT_NE(nullptr, body);
  ASSERT_EQ(ObString("text-embedding-v1"), print_node(allocator, body->get_value("model")));
  ObJsonNode *input_node = body->get_value("input");
  ASSERT_NE(nullptr, input_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, input_node->json_type());
  ObJsonArray *input_array = static_cast<ObJsonArray *>(input_node);
  ASSERT_EQ(2LL, input_array->element_count());
  ASSERT_EQ(ObString("first"), print_node(allocator, input_array->get_value(0)));
  ASSERT_EQ(ObString("second"), print_node(allocator, input_array->get_value(1)));

  // an empty input list is rejected
  ObArray<ObString> empty_contents;
  ObJsonObject *empty_body = nullptr;
  ASSERT_EQ(OB_INVALID_ARGUMENT, embed.get_body(allocator, model, empty_contents, nullptr, empty_body));
}

TEST_F(ObOpenAIUtilsTest, test_openai_embed_parse_output_in_order)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObJsonObject *response = build_json_object(allocator,
      "{\"data\":[{\"index\":0,\"embedding\":[1,2]},{\"index\":1,\"embedding\":[3,4]}]}");
  ASSERT_NE(nullptr, response);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  ObJsonArray *array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2LL, array->element_count());
  ASSERT_EQ(ObString("[1, 2]"), print_node(allocator, array->get_value(0)));
  ASSERT_EQ(ObString("[3, 4]"), print_node(allocator, array->get_value(1)));
}

TEST_F(ObOpenAIUtilsTest, test_openai_embed_parse_output_out_of_order)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  // the service answers in reverse order, the result must still follow the
  // original input order given by the "index" field
  ObJsonObject *response = build_json_object(allocator,
      "{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,2]}]}");
  ASSERT_NE(nullptr, response);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  ObJsonArray *array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2LL, array->element_count());
  ASSERT_EQ(ObString("[1, 2]"), print_node(allocator, array->get_value(0)));
  ASSERT_EQ(ObString("[3, 4]"), print_node(allocator, array->get_value(1)));
}

TEST_F(ObOpenAIUtilsTest, test_openai_embed_parse_output_without_index)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  // without an index the natural order of the response is kept
  ObJsonObject *response = build_json_object(allocator,
      "{\"data\":[{\"embedding\":[1,2]},{\"embedding\":[3,4]}]}");
  ASSERT_NE(nullptr, response);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator, response, result));
  ObJsonArray *array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2LL, array->element_count());
  ASSERT_EQ(ObString("[1, 2]"), print_node(allocator, array->get_value(0)));
  ASSERT_EQ(ObString("[3, 4]"), print_node(allocator, array->get_value(1)));
}

TEST_F(ObOpenAIUtilsTest, test_openai_embed_parse_output_invalid)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;

  // no data field at all
  ObJsonObject *no_data = build_json_object(allocator, "{\"foo\":1}");
  ASSERT_NE(nullptr, no_data);
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, no_data, result));

  // data is not an array
  ObJsonObject *not_array = build_json_object(allocator, "{\"data\":1}");
  ASSERT_NE(nullptr, not_array);
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, not_array, result));

  // an entry without the embedding field
  ObJsonObject *no_embedding = build_json_object(allocator, "{\"data\":[{\"index\":0}]}");
  ASSERT_NE(nullptr, no_embedding);
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, no_embedding, result));

  // an entry that is not an object
  ObJsonObject *bad_entry = build_json_object(allocator, "{\"data\":[[1,2]]}");
  ASSERT_NE(nullptr, bad_entry);
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, bad_entry, result));

  // a duplicated index leaves one position unfilled
  ObJsonObject *dup_index = build_json_object(allocator,
      "{\"data\":[{\"index\":0,\"embedding\":[1,2]},{\"index\":0,\"embedding\":[3,4]}]}");
  ASSERT_NE(nullptr, dup_index);
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, dup_index, result));

  // a null response is rejected
  ASSERT_EQ(OB_INVALID_ARGUMENT, embed.parse_output(allocator, nullptr, result));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_get_header)
{
  ObArenaAllocator allocator;
  ObArray<ObString> headers;
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObString api_key("mock-key");
  // the openai compatible provider does send an Authorization header
  ASSERT_EQ(OB_SUCCESS, embed.get_header(allocator, api_key, headers));
  ASSERT_EQ(2LL, headers.count());

  // ollama is self hosted and does not need any header
  ObOllamaUtils::ObOllamaComplete ollama_complete;
  ObArray<ObString> ollama_headers;
  ObString empty_key;
  ASSERT_EQ(OB_SUCCESS, ollama_complete.get_header(allocator, empty_key, ollama_headers));
  ASSERT_EQ(0LL, ollama_headers.count());
}

TEST_F(ObOpenAIUtilsTest, test_ollama_complete_request_and_response)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaComplete complete;
  ObString model("qwen2.5");
  ObString prompt;
  ObString content("hello");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator, model, prompt, content, nullptr, body));
  ASSERT_NE(nullptr, body);
  // the native protocol uses model + prompt instead of messages
  ASSERT_EQ(ObString("qwen2.5"), print_node(allocator, body->get_value("model")));
  ASSERT_EQ(ObString("hello"), print_node(allocator, body->get_value("prompt")));
  ASSERT_EQ(nullptr, body->get_value("messages"));

  ObJsonObject *response = build_json_object(allocator, "{\"response\":\"mock:hello\"}");
  ASSERT_NE(nullptr, response);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObString("mock:hello"), print_node(allocator, result));

  // an empty prompt is legal, an empty content is not
  ObJsonObject *empty_prompt_body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator, model, prompt, content, nullptr, empty_prompt_body));
  ObString empty_content;
  ObJsonObject *invalid_body = nullptr;
  ASSERT_EQ(OB_INVALID_ARGUMENT,
            complete.get_body(allocator, model, prompt, empty_content, nullptr, invalid_body));
}

TEST_F(ObOpenAIUtilsTest, test_ollama_embed_request_and_response)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaEmbed embed;
  ObString model("nomic-embed-text");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("hello")));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator, model, contents, nullptr, body));
  ASSERT_NE(nullptr, body);
  ASSERT_EQ(ObString("nomic-embed-text"), print_node(allocator, body->get_value("model")));
  ObJsonNode *input_node = body->get_value("input");
  ASSERT_NE(nullptr, input_node);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, input_node->json_type());
  ASSERT_EQ(1LL, static_cast<ObJsonArray *>(input_node)->element_count());

  ObJsonObject *response = build_json_object(allocator, "{\"embeddings\":[[1,2,3]]}");
  ASSERT_NE(nullptr, response);
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ObJsonArray *embeddings = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(1LL, embeddings->element_count());
  ASSERT_EQ(ObString("[1, 2, 3]"), print_node(allocator, embeddings->get_value(0)));

  // a response without embeddings cannot be used
  ObIJsonBase *bad_result = nullptr;
  ObJsonObject *bad_response = build_json_object(allocator, "{\"foo\":1}");
  ASSERT_NE(nullptr, bad_response);
  ASSERT_EQ(OB_ERR_UNEXPECTED, embed.parse_output(allocator, bad_response, bad_result));
}

} // namespace sql
} // namespace oceanbase
