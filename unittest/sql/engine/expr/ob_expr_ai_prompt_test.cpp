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

class ObExprAIPromptTest : public ::testing::Test
{
public:
  ObExprAIPromptTest() {}
  virtual ~ObExprAIPromptTest() {}
  virtual void SetUp() {}
  virtual void TearDown() {}
};

// Parse the json text of the argument list and build the prompt object
// {"template": template_str, "args": [...]} exactly like ai_prompt() does.
static int build_prompt_object(ObIAllocator &allocator,
                               const char *template_str,
                               const char *args_json,
                               ObJsonObject *&prompt_object)
{
  int ret = OB_SUCCESS;
  ObString template_ob_str(template_str);
  ObString args_str(args_json);
  ObIJsonBase *j_base = nullptr;
  ObJsonArray *args_array = nullptr;
  prompt_object = nullptr;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, args_str,
                                               ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FALSE_IT(args_array = static_cast<ObJsonArray *>(j_base))) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(
                       allocator, template_ob_str, args_array, prompt_object))) {
  }
  return ret;
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

TEST_F(ObExprAIPromptTest, test_construct_prompt_object)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "hello {0}", "[\"world\"]", prompt_object));
  ASSERT_NE(nullptr, prompt_object);
  // the prompt object is exactly template + args
  ASSERT_EQ(2LL, prompt_object->element_count());
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ASSERT_EQ(ObString("hello {0}"),
            print_node(allocator, prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key)));
  ObJsonArray *args = static_cast<ObJsonArray *>(
      prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ASSERT_NE(nullptr, args);
  ASSERT_EQ(1LL, args->element_count());
  ASSERT_EQ(ObJsonNodeType::J_STRING, args->get_value(0)->json_type());
}

TEST_F(ObExprAIPromptTest, test_empty_template_is_rejected)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  // an empty template cannot produce a prompt at all
  ASSERT_EQ(OB_INVALID_ARGUMENT, build_prompt_object(allocator, "", "[]", prompt_object));
  ASSERT_EQ(nullptr, prompt_object);
}

TEST_F(ObExprAIPromptTest, test_json_argument_keeps_structure)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "describe {0}", "[{\"text\":\"hi\"}]", prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ObJsonArray *args = static_cast<ObJsonArray *>(
      prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ASSERT_NE(nullptr, args);
  ASSERT_EQ(1LL, args->element_count());
  // a json object argument is stored as an object, not flattened into a string
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, args->get_value(0)->json_type());
  // and it is substituted with its json representation
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
  ASSERT_EQ(ObString("describe {\"text\": \"hi\"}"), replaced);
}

TEST_F(ObExprAIPromptTest, test_json_array_argument_keeps_structure)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "{0}", "[[1,true]]", prompt_object));
  ObJsonArray *args = static_cast<ObJsonArray *>(
      prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ASSERT_NE(nullptr, args);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, args->get_value(0)->json_type());
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
  ASSERT_EQ(ObString("[1, true]"), replaced);
}

TEST_F(ObExprAIPromptTest, test_duplicate_placeholder_is_replaced_everywhere)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "{0}/{0}/{1}", "[[1,true],{\"k\":2}]", prompt_object));
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
  // every occurrence of {0} is replaced, not only the first one
  ASSERT_EQ(ObString("[1, true]/[1, true]/{\"k\": 2}"), replaced);
}

TEST_F(ObExprAIPromptTest, test_string_argument_is_substituted_raw)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  // an empty string is a legal argument value
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "1+1=?", "[\"\"]", prompt_object));
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
  // no placeholder in the template, the template is returned unchanged
  ASSERT_EQ(ObString("1+1=?"), replaced);

  // string arguments contribute their raw characters, without json quoting
  ObJsonObject *quoted = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "say {0}", "[\"hi\"]", quoted));
  ObString quoted_replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, quoted, quoted_replaced));
  ASSERT_EQ(ObString("say hi"), quoted_replaced);
}

TEST_F(ObExprAIPromptTest, test_nested_prompt_object_is_substituted)
{
  ObArenaAllocator allocator;
  ObJsonObject *inner = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "inner {0}", "[\"x\"]", inner));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(inner));

  // the nested prompt object is used as the argument of an outer prompt
  ObString outer_template("outer {0}");
  ObJsonArray *args = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args));
  ASSERT_EQ(OB_SUCCESS, args->append(inner));
  ObJsonObject *outer = nullptr;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, outer_template, args, outer));
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, outer, replaced));
  ASSERT_EQ(ObString("outer {\"template\": \"inner {0}\", \"args\": [\"x\"]}"), replaced);
}

TEST_F(ObExprAIPromptTest, test_invalid_placeholder_index)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  // {2} has no matching argument
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "{0} {2}", "[\"a\"]", prompt_object));
  ObString replaced;
  ASSERT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
}

TEST_F(ObExprAIPromptTest, test_placeholder_without_index_is_literal)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  // "{}" and "{a}" are not placeholders and stay in the template
  ASSERT_EQ(OB_SUCCESS, build_prompt_object(allocator, "{} {a} {0}", "[\"z\"]", prompt_object));
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt_object, replaced));
  ASSERT_EQ(ObString("{} {a} z"), replaced);
}

TEST_F(ObExprAIPromptTest, test_is_valid_prompt_object_rejects_malformed_input)
{
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(nullptr));

  ObArenaAllocator allocator;
  ObIJsonBase *j_base = nullptr;
  ObString malformed("{\"template\": 1, \"args\": []}");
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, malformed,
                                                         ObJsonInType::JSON_TREE,
                                                         ObJsonInType::JSON_TREE, j_base));
  ASSERT_NE(nullptr, j_base);
  // the template must be a string
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(j_base)));

  ObIJsonBase *missing_args = nullptr;
  ObString only_template("{\"template\": \"a\"}");
  ASSERT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator, only_template,
                                                         ObJsonInType::JSON_TREE,
                                                         ObJsonInType::JSON_TREE, missing_args));
  ASSERT_NE(nullptr, missing_args);
  // the args key is mandatory
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(missing_args)));
}

} // namespace sql
} // namespace oceanbase
