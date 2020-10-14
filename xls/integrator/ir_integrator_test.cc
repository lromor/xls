// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/integrator/ir_integrator.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"

namespace xls {
namespace {

using ::testing::UnorderedElementsAre;

class IntegratorTest : public IrTestBase {};

TEST_F(IntegratorTest, NoSourceFunctions) {
  IntegrationBuilder builder({});
  EXPECT_FALSE(builder.Build().ok());
}

TEST_F(IntegratorTest, OneSourceFunction) {
  std::string program = R"(package dot

fn __dot__add(a: bits[32], b: bits[32]) -> bits[32] {
  ret add.3: bits[32] = add(a, b, id=3, pos=0,1,4)
}

fn ____dot__main_counted_for_0_body(idx: bits[32], acc: bits[32], a: bits[32][3], b: bits[32][3]) -> bits[32] {
  array_index.12: bits[32] = array_index(a, idx, id=12, pos=0,6,16)
  array_index.13: bits[32] = array_index(b, idx, id=13, pos=0,6,25)
  umul.14: bits[32] = umul(array_index.12, array_index.13, id=14, pos=0,6,22)
  ret invoke.15: bits[32] = invoke(acc, umul.14, to_apply=__dot__add, id=15, pos=0,7,7)
}

fn __dot__main(a: bits[32][3], b: bits[32][3]) -> bits[32] {
  literal.6: bits[32] = literal(value=0, id=6, pos=0,8,10)
  literal.7: bits[32] = literal(value=3, id=7, pos=0,5,49)
  ret counted_for.16: bits[32] = counted_for(literal.6, trip_count=3, stride=1, body=____dot__main_counted_for_0_body, invariant_args=[a, b], id=16, pos=0,5,5)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(auto p, Parser::ParsePackage(program));
  IntegrationBuilder builder({p->EntryFunction().value()});
  XLS_ASSERT_OK_AND_ASSIGN(auto integration_func, builder.Build());

  // Integrated function is just the original entry function.
  EXPECT_EQ(
      integration_func,
      builder.package()->GetFunction("PKGzzzdotzzzFNzzz__dot__main").value());

  auto get_function_names = [](Package* p) {
    std::vector<std::string> names;
    for (const auto& func : p->functions()) {
      names.push_back(func->name());
    }
    return names;
  };

  auto get_called_function_names = [](Package* p) {
    std::vector<std::string> names;
    for (const auto& func : p->functions()) {
      for (const auto* node : func->nodes()) {
        if (node->op() == Op::kCountedFor) {
          names.push_back(node->As<CountedFor>()->body()->name());
        }
        if (node->op() == Op::kInvoke) {
          names.push_back(node->As<Invoke>()->to_apply()->name());
        }
      }
    }
    return names;
  };

  // Original package is unchanged.
  EXPECT_THAT(
      get_function_names(p.get()),
      UnorderedElementsAre("__dot__add", "____dot__main_counted_for_0_body",
                           "__dot__main"));
  EXPECT_THAT(
      get_called_function_names(p.get()),
      UnorderedElementsAre("__dot__add", "____dot__main_counted_for_0_body"));

  // IntegrationBuilder package uses qualified function names (including calls).
  EXPECT_THAT(
      get_function_names(builder.package()),
      UnorderedElementsAre("PKGzzzdotzzzFNzzz__dot__add",
                           "PKGzzzdotzzzFNzzz____dot__main_counted_for_0_body",
                           "PKGzzzdotzzzFNzzz__dot__main"));
  EXPECT_THAT(get_called_function_names(builder.package()),
              UnorderedElementsAre(
                  "PKGzzzdotzzzFNzzz__dot__add",
                  "PKGzzzdotzzzFNzzz____dot__main_counted_for_0_body"));
}

}  // namespace
}  // namespace xls
