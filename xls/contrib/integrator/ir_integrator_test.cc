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

#include "xls/contrib/integrator/ir_integrator.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/node_iterator.h"
#include "xls/ir/package.h"
#include "xls/ir/verifier.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;
using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

class IntegratorTest : public IrTestBase {};

TEST_F(IntegratorTest, NoSourceFunctions) {
  EXPECT_FALSE(IntegrationBuilder::Build({}).ok());
}

TEST_F(IntegratorTest, OneSourceFunction) {
  auto p = CreatePackage();

  FunctionBuilder fb_body("body", p.get());
  fb_body.Param("index", p->GetBitsType(2));
  fb_body.Param("acc", p->GetBitsType(2));
  fb_body.Literal(UBits(0b11, 2));
  XLS_ASSERT_OK_AND_ASSIGN(Function * body_func, fb_body.Build());

  FunctionBuilder fb_double("double", p.get());
  auto double_in = fb_double.Param("in1", p->GetBitsType(2));
  fb_double.Add(double_in, double_in);
  XLS_ASSERT_OK_AND_ASSIGN(Function * double_func, fb_double.Build());

  FunctionBuilder fb_main("main", p.get());
  auto main_in1 = fb_main.Param("in1", p->GetBitsType(2));
  auto main_in_arr =
      fb_main.Param("in_arr", p->GetArrayType(2, p->GetBitsType(2)));
  fb_main.Map(main_in_arr, double_func, /*loc=*/std::nullopt, "map");
  fb_main.CountedFor(main_in1, /*trip_count=*/4, /*stride=*/1, body_func,
                     /*invariant_args=*/{}, /*loc=*/std::nullopt,
                     "counted_for");
  fb_main.Invoke(/*args=*/{main_in1}, double_func, /*loc=*/std::nullopt,
                 "invoke");
  XLS_ASSERT_OK_AND_ASSIGN(Function * main_func, fb_main.Build());

  auto get_function_names = [](Package* p) {
    std::vector<std::string> names;
    for (const auto& func : p->functions()) {
      names.push_back(func->name());
    }
    return names;
  };

  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<IntegrationBuilder> builder,
                           IntegrationBuilder::Build({main_func}));

  // Original package is unchanged.
  EXPECT_THAT(get_function_names(p.get()),
              UnorderedElementsAre("body", "double", "main"));

  // Original CountedFor
  CountedFor* counted_original =
      FindNode("counted_for", main_func)->As<CountedFor>();
  EXPECT_EQ(counted_original->body(), body_func);
  EXPECT_EQ(counted_original->body()->name(), "body");
  EXPECT_EQ(counted_original->body()->package(), p.get());

  // Original Map
  Map* map_original = FindNode("map", main_func)->As<Map>();
  EXPECT_EQ(map_original->to_apply(), double_func);
  EXPECT_EQ(map_original->to_apply()->name(), "double");
  EXPECT_EQ(map_original->to_apply()->package(), p.get());

  // Original Invoke
  Invoke* invoke_original = FindNode("invoke", main_func)->As<Invoke>();
  EXPECT_EQ(invoke_original->to_apply(), double_func);
  EXPECT_EQ(invoke_original->to_apply()->name(), "double");
  EXPECT_EQ(invoke_original->to_apply()->package(), p.get());

  // builder package has copies of functions.
  EXPECT_THAT(get_function_names(builder->package()),
              UnorderedElementsAre("body", "double", "main"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * clone_body_func,
                           builder->package()->GetFunction("body"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * clone_double_func,
                           builder->package()->GetFunction("double"));

  // Clone CountedFor
  Function* main_clone_func = builder->integrated_function();
  CountedFor* counted_clone =
      FindNode("counted_for", main_clone_func)->As<CountedFor>();
  EXPECT_EQ(counted_clone->body(), clone_body_func);
  EXPECT_EQ(counted_clone->body()->name(), "body");
  EXPECT_EQ(counted_clone->body()->package(), builder->package());

  // Clone Map
  Map* map_clone = FindNode("map", main_clone_func)->As<Map>();
  EXPECT_EQ(map_clone->to_apply(), clone_double_func);
  EXPECT_EQ(map_clone->to_apply()->name(), "double");
  EXPECT_EQ(map_clone->to_apply()->package(), builder->package());

  // Clone Invoke
  Invoke* invoke_clone = FindNode("invoke", main_clone_func)->As<Invoke>();
  EXPECT_EQ(invoke_clone->to_apply(), clone_double_func);
  EXPECT_EQ(invoke_clone->to_apply()->name(), "double");
  EXPECT_EQ(invoke_clone->to_apply()->package(), builder->package());
}

TEST_F(IntegratorTest, MappingTestSimple) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      IntegrationFunction::MakeIntegrationFunctionWithParamTuples(p.get(), {}));
  Function& internal_func = *integration->function();
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(2)));

  // Before mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_FALSE(integration->HasMapping(external_1));
  EXPECT_FALSE(integration->IsMappingTarget(internal_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(external_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(internal_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());

  // Mapping = external_1 -> MapsTo -> internal_1
  XLS_ASSERT_OK(integration->SetNodeMapping(external_1, internal_1));

  // After mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_TRUE(integration->HasMapping(external_1));
  EXPECT_TRUE(integration->IsMappingTarget(internal_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  ASSERT_THAT(integration->GetNodeMapping(external_1),
              IsOkAndHolds(internal_1));
  auto mapped_to_internal_1 = integration->GetNodesMappedToNode(internal_1);
  EXPECT_TRUE(mapped_to_internal_1.ok());
  EXPECT_THAT(*(mapped_to_internal_1.value()),
              UnorderedElementsAre(external_1));
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());
}

TEST_F(IntegratorTest, MappingTestMultipleNodesMapToTarget) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      IntegrationFunction::MakeIntegrationFunctionWithParamTuples(p.get(), {}));
  Function& internal_func = *integration->function();
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_2,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(3)));

  // Before mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_2));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_FALSE(integration->HasMapping(external_1));
  EXPECT_FALSE(integration->HasMapping(external_2));
  EXPECT_FALSE(integration->IsMappingTarget(internal_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_2));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(external_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(external_2).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(internal_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_2).ok());

  // Mapping = external_1 && external_2 -> MapsTo -> internal_1
  XLS_ASSERT_OK(integration->SetNodeMapping(external_1, internal_1));
  XLS_ASSERT_OK(integration->SetNodeMapping(external_2, internal_1));

  // After mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_2));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_TRUE(integration->HasMapping(external_1));
  EXPECT_TRUE(integration->HasMapping(external_2));
  EXPECT_TRUE(integration->IsMappingTarget(internal_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_2));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  ASSERT_THAT(integration->GetNodeMapping(external_1),
              IsOkAndHolds(internal_1));
  ASSERT_THAT(integration->GetNodeMapping(external_2),
              IsOkAndHolds(internal_1));
  auto mapped_to_internal_1 = integration->GetNodesMappedToNode(internal_1);
  EXPECT_TRUE(mapped_to_internal_1.ok());
  EXPECT_THAT(*(mapped_to_internal_1.value()),
              UnorderedElementsAre(external_1, external_2));
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_2).ok());
}

TEST_F(IntegratorTest, MappingTestRepeatedMapping) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      IntegrationFunction::MakeIntegrationFunctionWithParamTuples(p.get(), {}));
  Function& internal_func = *integration->function();
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(3)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_2,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_2",
                                            p->GetBitsType(4)));

  // Before mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_2));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_2));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_FALSE(integration->HasMapping(internal_2));
  EXPECT_FALSE(integration->HasMapping(external_1));
  EXPECT_FALSE(integration->HasMapping(external_2));
  EXPECT_FALSE(integration->IsMappingTarget(internal_1));
  EXPECT_FALSE(integration->IsMappingTarget(internal_2));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_2));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(internal_2).ok());
  EXPECT_FALSE(integration->GetNodeMapping(external_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(external_2).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(internal_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(internal_2).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_2).ok());

  // Mapping = external_1 && external_2 -> MapsTo -> internal_1
  XLS_ASSERT_OK(integration->SetNodeMapping(external_1, internal_1));
  XLS_ASSERT_OK(integration->SetNodeMapping(external_2, internal_1));

  // Mapping = external_1 && external_2 -> MapsTo -> internal_1 -> internal_2
  XLS_ASSERT_OK(integration->SetNodeMapping(internal_1, internal_2));

  // After mapping.
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_1));
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(internal_2));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_1));
  EXPECT_FALSE(integration->IntegrationFunctionOwnsNode(external_2));
  EXPECT_FALSE(integration->HasMapping(internal_1));
  EXPECT_FALSE(integration->HasMapping(internal_2));
  EXPECT_TRUE(integration->HasMapping(external_1));
  EXPECT_TRUE(integration->HasMapping(external_2));
  EXPECT_FALSE(integration->IsMappingTarget(internal_1));
  EXPECT_TRUE(integration->IsMappingTarget(internal_2));
  EXPECT_FALSE(integration->IsMappingTarget(external_1));
  EXPECT_FALSE(integration->IsMappingTarget(external_2));
  EXPECT_FALSE(integration->GetNodeMapping(internal_1).ok());
  EXPECT_FALSE(integration->GetNodeMapping(internal_2).ok());
  ASSERT_THAT(integration->GetNodeMapping(external_1),
              IsOkAndHolds(internal_2));
  ASSERT_THAT(integration->GetNodeMapping(external_2),
              IsOkAndHolds(internal_2));
  EXPECT_FALSE(integration->GetNodesMappedToNode(internal_1).ok());
  auto mapped_to_internal_2 = integration->GetNodesMappedToNode(internal_2);
  EXPECT_TRUE(mapped_to_internal_2.ok());
  EXPECT_THAT(*(mapped_to_internal_2.value()),
              UnorderedElementsAre(external_1, external_2));
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_1).ok());
  EXPECT_FALSE(integration->GetNodesMappedToNode(external_2).ok());
}

TEST_F(IntegratorTest, MappingTestSetNodeMappingFailureCases) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      IntegrationFunction::MakeIntegrationFunctionWithParamTuples(p.get(), {}));
  Function& internal_func = *integration->function();
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(3)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_2,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_2",
                                            p->GetBitsType(4)));

  // Mapping = external_1 -> MapsTo -> external_1
  // Mapping target must be internal.
  EXPECT_FALSE(integration->SetNodeMapping(external_1, external_1).ok());

  // Mapping = external_1 -> MapsTo -> external_2
  // Mapping target must be internal.
  EXPECT_FALSE(integration->SetNodeMapping(external_1, external_2).ok());

  // Mapping = internal_1 -> MapsTo -> external_1
  // Mapping target must be internal.
  EXPECT_FALSE(integration->SetNodeMapping(internal_1, external_1).ok());

  // Mapping = internal_1 -> MapsTo -> internal_1
  // Cannot map to self.
  EXPECT_FALSE(integration->SetNodeMapping(internal_1, internal_1).ok());

  // Mapping = internal_2 -> MapsTo -> internal_1
  // Cannot map internal nodes that are not mapping targets.
  EXPECT_FALSE(integration->SetNodeMapping(internal_2, internal_1).ok());
}

TEST_F(IntegratorTest, ParamterPacking) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  fb_a.Param("a1", p->GetBitsType(2));
  fb_a.Param("a2", p->GetBitsType(4));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  FunctionBuilder fb_b("func_b", p.get());
  fb_b.Param("b1", p->GetBitsType(6));
  fb_b.Param("b2", p->GetBitsType(8));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  auto GetTupleIndexWithNumBits = [&](int64 num_bits) {
    for (Node* node : integration->function()->nodes()) {
      if (node->op() == Op::kTupleIndex) {
        if (node->GetType() == p->GetBitsType(num_bits)) {
          return absl::optional<Node*>(node);
        }
      }
    }
    return absl::optional<Node*>(std::nullopt);
  };
  auto GetParamWithNumBits = [&p](Function* function, int64 num_bits) {
    for (Node* node : function->nodes()) {
      if (node->op() == Op::kParam) {
        if (node->GetType() == p->GetBitsType(num_bits)) {
          return absl::optional<Node*>(node);
        }
      }
    }
    return absl::optional<Node*>(std::nullopt);
  };

  auto a1_index = GetTupleIndexWithNumBits(2);
  EXPECT_TRUE(a1_index.has_value());
  EXPECT_TRUE(a1_index.has_value());
  EXPECT_THAT(a1_index.value(), m::TupleIndex(m::Param("func_aParamTuple"), 0));
  auto a1_source = GetParamWithNumBits(func_a, 2);
  EXPECT_TRUE(a1_source.has_value());
  EXPECT_TRUE(integration->HasMapping(a1_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(a1_source.value()).value(),
            a1_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(a1_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(a1_index.value()).value()),
              UnorderedElementsAre(a1_source.value()));

  auto a2_index = GetTupleIndexWithNumBits(4);
  EXPECT_TRUE(a2_index.has_value());
  EXPECT_THAT(a2_index.value(), m::TupleIndex(m::Param("func_aParamTuple"), 1));
  auto a2_source = GetParamWithNumBits(func_a, 4);
  EXPECT_TRUE(a2_source.has_value());
  EXPECT_TRUE(integration->HasMapping(a2_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(a2_source.value()).value(),
            a2_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(a2_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(a2_index.value()).value()),
              UnorderedElementsAre(a2_source.value()));

  auto b1_index = GetTupleIndexWithNumBits(6);
  EXPECT_TRUE(b1_index.has_value());
  EXPECT_THAT(b1_index.value(), m::TupleIndex(m::Param("func_bParamTuple"), 0));
  auto b1_source = GetParamWithNumBits(func_b, 6);
  EXPECT_TRUE(b1_source.has_value());
  EXPECT_TRUE(integration->HasMapping(b1_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(b1_source.value()).value(),
            b1_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(b1_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(b1_index.value()).value()),
              UnorderedElementsAre(b1_source.value()));

  auto b2_index = GetTupleIndexWithNumBits(8);
  EXPECT_TRUE(b2_index.has_value());
  EXPECT_THAT(b2_index.value(), m::TupleIndex(m::Param("func_bParamTuple"), 1));
  auto b2_source = GetParamWithNumBits(func_b, 8);
  EXPECT_TRUE(b2_source.has_value());
  EXPECT_TRUE(integration->HasMapping(b2_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(b2_source.value()).value(),
            b2_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(b2_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(b2_index.value()).value()),
              UnorderedElementsAre(b2_source.value()));

  EXPECT_EQ(integration->function()->node_count(), 6);
}

TEST_F(IntegratorTest, ParamterPackingUniversalMuxSelect) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  fb_a.Param("a1", p->GetBitsType(2));
  fb_a.Param("a2", p->GetBitsType(4));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  FunctionBuilder fb_b("func_b", p.get());
  fb_b.Param("b1", p->GetBitsType(6));
  fb_b.Param("b2", p->GetBitsType(8));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  auto GetTupleIndexWithNumBits = [&](long int num_bits) {
    for (Node* node : integration->function()->nodes()) {
      if (node->op() == Op::kTupleIndex) {
        if (node->GetType() == p->GetBitsType(num_bits)) {
          return absl::optional<Node*>(node);
        }
      }
    }
    return absl::optional<Node*>(std::nullopt);
  };
  auto GetParamWithNumBits = [&p](Function* function, long int num_bits) {
    for (Node* node : function->nodes()) {
      if (node->op() == Op::kParam) {
        if (node->GetType() == p->GetBitsType(num_bits)) {
          return absl::optional<Node*>(node);
        }
      }
    }
    return absl::optional<Node*>(std::nullopt);
  };

  auto a1_index = GetTupleIndexWithNumBits(2);
  EXPECT_TRUE(a1_index.has_value());
  EXPECT_TRUE(a1_index.has_value());
  EXPECT_THAT(a1_index.value(), m::TupleIndex(m::Param("func_aParamTuple"), 0));
  auto a1_source = GetParamWithNumBits(func_a, 2);
  EXPECT_TRUE(a1_source.has_value());
  EXPECT_TRUE(integration->HasMapping(a1_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(a1_source.value()).value(),
            a1_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(a1_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(a1_index.value()).value()),
              UnorderedElementsAre(a1_source.value()));

  auto a2_index = GetTupleIndexWithNumBits(4);
  EXPECT_TRUE(a2_index.has_value());
  EXPECT_THAT(a2_index.value(), m::TupleIndex(m::Param("func_aParamTuple"), 1));
  ;
  auto a2_source = GetParamWithNumBits(func_a, 4);
  EXPECT_TRUE(a2_source.has_value());
  EXPECT_TRUE(integration->HasMapping(a2_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(a2_source.value()).value(),
            a2_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(a2_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(a2_index.value()).value()),
              UnorderedElementsAre(a2_source.value()));

  auto b1_index = GetTupleIndexWithNumBits(6);
  EXPECT_TRUE(b1_index.has_value());
  EXPECT_THAT(b1_index.value(), m::TupleIndex(m::Param("func_bParamTuple"), 0));
  ;
  auto b1_source = GetParamWithNumBits(func_b, 6);
  EXPECT_TRUE(b1_source.has_value());
  EXPECT_TRUE(integration->HasMapping(b1_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(b1_source.value()).value(),
            b1_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(b1_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(b1_index.value()).value()),
              UnorderedElementsAre(b1_source.value()));

  auto b2_index = GetTupleIndexWithNumBits(8);
  EXPECT_TRUE(b2_index.has_value());
  EXPECT_THAT(b2_index.value(), m::TupleIndex(m::Param("func_bParamTuple"), 1));
  ;
  auto b2_source = GetParamWithNumBits(func_b, 8);
  EXPECT_TRUE(b2_source.has_value());
  EXPECT_TRUE(integration->HasMapping(b2_source.value()));
  EXPECT_EQ(integration->GetNodeMapping(b2_source.value()).value(),
            b2_index.value());
  EXPECT_TRUE(integration->IsMappingTarget(b2_index.value()));
  EXPECT_THAT(*(integration->GetNodesMappedToNode(b2_index.value()).value()),
              UnorderedElementsAre(b2_source.value()));

  EXPECT_EQ(integration->function()->node_count(), 7);
  auto global_mux_select =
      FindNode("global_mux_select", integration->function());
  EXPECT_EQ(global_mux_select->GetType(), p->GetBitsType(1));
}

TEST_F(IntegratorTest, GetIntegratedOperandsExternalNode) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a1 = fb_a.Param("a1", p->GetBitsType(2));
  auto a2 = fb_a.Param("a2", p->GetBitsType(2));
  fb_a.Add(a1, a2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a})));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a1_node, func_a->GetNode("a1"));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a2_node, func_a->GetNode("a2"));
  Node* add_node = func_a->return_value();

  XLS_ASSERT_OK_AND_ASSIGN(Node * a1_map_target,
                           integration->GetNodeMapping(a1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a2_map_target,
                           integration->GetNodeMapping(a2_node));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Node*> operand_mappings,
                           integration->GetIntegratedOperands(add_node));
  EXPECT_THAT(operand_mappings, ElementsAre(a1_map_target, a2_map_target));
}

TEST_F(IntegratorTest, GetIntegratedOperandsInternalNode) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {})));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * cat,
      internal_func.MakeNode<Concat>(
          /*loc=*/std::nullopt, std::vector<Node*>({internal_1, internal_2})));

  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Node*> operand_mappings,
                           integration->GetIntegratedOperands(cat));
  EXPECT_THAT(operand_mappings, ElementsAre(internal_1, internal_2));
}

TEST_F(IntegratorTest, UnifyIntegrationNodesSameNode) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));

  int64 initial_node_count = integration->function()->node_count();
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * unity, integration->UnifyIntegrationNodes(internal_1, internal_1));
  EXPECT_EQ(unity, internal_1);
  EXPECT_EQ(initial_node_count, integration->function()->node_count());
}

TEST_F(IntegratorTest, UnifyIntegrationNodesSameNodeCheckMuxAdded) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));

  int64 initial_node_count = integration->function()->node_count();
  bool new_mux = true;
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity, integration->UnifyIntegrationNodes(
                                             internal_1, internal_1, &new_mux));
  EXPECT_FALSE(new_mux);
  EXPECT_EQ(unity, internal_1);
  EXPECT_EQ(initial_node_count, integration->function()->node_count());
}

TEST_F(IntegratorTest, UnifyIntegrationNodesDifferentNodes) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));

  int64 initial_node_count = integration->function()->node_count();
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * unity, integration->UnifyIntegrationNodes(internal_1, internal_2));
  std::string select_name =
      internal_1->GetName() + "_" + internal_2->GetName() + "_mux_sel";
  XLS_ASSERT_OK_AND_ASSIGN(Node * mux_sel,
                           integration->function()->GetNode(select_name));
  EXPECT_EQ(mux_sel->users().size(), 1);
  Node* mux = mux_sel->users().at(0);
  EXPECT_EQ(unity, mux);
  EXPECT_THAT(unity,
              m::Select(m::Param(select_name),
                        {m::Param("internal_1"), m::Param("internal_2")}));
  EXPECT_EQ(initial_node_count + 2, integration->function()->node_count());
}

TEST_F(IntegratorTest, UnifyIntegrationNodesDifferentNodesRepeated) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));

  int64 initial_node_count = integration->function()->node_count();
  for (int64 i = 0; i < 10; ++i) {
    XLS_ASSERT_OK(integration->UnifyIntegrationNodes(internal_1, internal_2));
  }
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * unity, integration->UnifyIntegrationNodes(internal_1, internal_2));
  std::string select_name =
      internal_1->GetName() + "_" + internal_2->GetName() + "_mux_sel";
  XLS_ASSERT_OK_AND_ASSIGN(Node * mux_sel,
                           integration->function()->GetNode(select_name));
  EXPECT_EQ(mux_sel->users().size(), 1);
  Node* mux = mux_sel->users().at(0);
  EXPECT_EQ(unity, mux);
  EXPECT_THAT(unity,
              m::Select(m::Param(select_name),
                        {m::Param("internal_1"), m::Param("internal_2")}));
  EXPECT_EQ(initial_node_count + 2, integration->function()->node_count());
}

TEST_F(IntegratorTest,
       UnifyIntegrationNodesDifferentNodesRepeatedCheckMuxAdded) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));

  int64 initial_node_count = integration->function()->node_count();
  bool mux_added = false;
  XLS_ASSERT_OK(
      integration->UnifyIntegrationNodes(internal_1, internal_2, &mux_added));
  EXPECT_TRUE(mux_added);

  mux_added = true;
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * unity,
      integration->UnifyIntegrationNodes(internal_1, internal_2, &mux_added));
  EXPECT_FALSE(mux_added);
  std::string select_name =
      internal_1->GetName() + "_" + internal_2->GetName() + "_mux_sel";
  XLS_ASSERT_OK_AND_ASSIGN(Node * mux_sel,
                           integration->function()->GetNode(select_name));
  EXPECT_EQ(mux_sel->users().size(), 1);
  Node* mux = mux_sel->users().at(0);
  EXPECT_EQ(unity, mux);
  EXPECT_THAT(unity,
              m::Select(m::Param(select_name),
                        {m::Param("internal_1"), m::Param("internal_2")}));
  EXPECT_EQ(initial_node_count + 2, integration->function()->node_count());
}

TEST_F(IntegratorTest, UnifyIntegrationNodesDifferentNodesFailureCases) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(1)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(2)));

  // Can't unify with external node.
  EXPECT_FALSE(integration->UnifyIntegrationNodes(internal_1, external_1).ok());
  EXPECT_FALSE(integration->UnifyIntegrationNodes(external_1, internal_1).ok());

  // Unifying types must match.
  EXPECT_FALSE(integration->UnifyIntegrationNodes(internal_2, internal_1).ok());
}

TEST_F(IntegratorTest, UnifyIntegrationGlobalSelectCreateMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  Node* a_add_node = FindNode("add", func_a);
  Node* c_add_node = FindNode("add", func_c);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));

  EXPECT_EQ(integration->function()->node_count(), 9);
  bool mux_added = false;
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity,
                           integration->UnifyIntegrationNodes(
                               a_add_internal, c_add_internal, &mux_added));

  // Check added mux.
  EXPECT_TRUE(mux_added);
  EXPECT_EQ(integration->function()->node_count(), 10);
  EXPECT_EQ(unity->op(), Op::kSel);
  Select* mux = unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), a_add_internal);
  EXPECT_THAT(mux->cases(),
              ElementsAre(a_add_internal, a_add_internal, c_add_internal));

  // Check bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(int64 muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 1);
  XLS_ASSERT_OK_AND_ASSIGN(const std::set<int64>* occupied,
                           integration->GetGlobalMuxOccupiedCaseIndexes(mux));
  EXPECT_THAT(*occupied, ElementsAre(0, 2));
  XLS_ASSERT_OK_AND_ASSIGN(const std::set<int64>* last_assigned,
                           integration->GetGlobalMuxLastCaseIndexesAdded(mux));
  EXPECT_THAT(*last_assigned, ElementsAre(0, 2));
}

TEST_F(IntegratorTest, UnifyIntegrationGlobalSelectModifyMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_d, func_a->Clone("func_d"));
  Node* a_add_node = FindNode("add", func_a);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_add_node = FindNode("add", func_c);
  Node* d_add_node = FindNode("add", func_d);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c, func_d},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_add_internal,
                           integration->InsertNode(b_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * d_add_internal,
                           integration->InsertNode(d_add_node));

  EXPECT_EQ(integration->function()->node_count(), 13);
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity, integration->UnifyIntegrationNodes(
                                             a_add_internal, c_add_internal));
  XLS_ASSERT_OK_AND_ASSIGN(
      unity, integration->UnifyIntegrationNodes(unity, d_add_internal));

  // Create another mux separate from the one we're modifying.
  XLS_ASSERT_OK_AND_ASSIGN(Node * other, integration->UnifyIntegrationNodes(
                                             a_add_internal, c_add_internal));

  bool mux_added = true;
  XLS_ASSERT_OK_AND_ASSIGN(unity, integration->UnifyIntegrationNodes(
                                      unity, b_add_internal, &mux_added));

  // Check added muxes.
  EXPECT_FALSE(mux_added);
  EXPECT_EQ(integration->function()->node_count(), 15);
  // Modified mux.
  EXPECT_EQ(unity->op(), Op::kSel);
  Select* mux = unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, b_add_internal,
                                        c_add_internal, d_add_internal));
  // Other mux.
  EXPECT_EQ(other->op(), Op::kSel);
  Select* other_mux = other->As<Select>();
  EXPECT_EQ(other_mux->selector(), integration->global_mux_select());
  EXPECT_EQ(other_mux->default_value(), std::nullopt);
  EXPECT_THAT(other_mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                              c_add_internal, a_add_internal));

  // Check bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(int64 muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);
  // Modified mux.
  XLS_ASSERT_OK_AND_ASSIGN(const std::set<int64>* occupied,
                           integration->GetGlobalMuxOccupiedCaseIndexes(mux));
  EXPECT_THAT(*occupied, ElementsAre(0, 1, 2, 3));
  XLS_ASSERT_OK_AND_ASSIGN(const std::set<int64>* last_assigned,
                           integration->GetGlobalMuxLastCaseIndexesAdded(mux));
  EXPECT_THAT(*last_assigned, ElementsAre(1));
  // Other mux.
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* other_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(other_mux));
  EXPECT_THAT(*other_occupied, ElementsAre(0, 2));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* other_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(other_mux));
  EXPECT_THAT(*other_last_assigned, ElementsAre(0, 2));
}

TEST_F(IntegratorTest, UnifyIntegrationGlobalSelectErrorCases) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add2");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_d, func_a->Clone("func_d"));
  Node* a_add_node = FindNode("add", func_a);
  Node* a_add2_node = FindNode("add2", func_a);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_add_node = FindNode("add", func_c);
  Node* d_add_node = FindNode("add", func_d);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c, func_d},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add2_internal,
                           integration->InsertNode(a_add2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_add_internal,
                           integration->InsertNode(b_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * d_add_internal,
                           integration->InsertNode(d_add_node));

  // Try to unify two muxes.
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity1, integration->UnifyIntegrationNodes(
                                              a_add_internal, c_add_internal));
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity2, integration->UnifyIntegrationNodes(
                                              b_add_internal, d_add_internal));
  auto unify_mux_result = integration->UnifyIntegrationNodes(unity1, unity2);
  EXPECT_FALSE(unify_mux_result.ok());

  // Conflicting case assignments
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity3, integration->UnifyIntegrationNodes(
                                              a_add_internal, c_add_internal));
  auto conflicitng_case_mux_result =
      integration->UnifyIntegrationNodes(unity3, a_add2_internal);
  EXPECT_FALSE(conflicitng_case_mux_result.ok());

  // Try to unify w/ node that is not a map target.
  XLS_ASSERT_OK_AND_ASSIGN(Node * unity4, integration->UnifyIntegrationNodes(
                                              a_add_internal, c_add_internal));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      integration->function()->MakeNodeWithName<Param>(
          /*loc=*/std::nullopt, "internal_1", p->GetBitsType(2)));
  auto no_map_mux_result =
      integration->UnifyIntegrationNodes(unity4, internal_1);
  EXPECT_FALSE(no_map_mux_result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationGlobalSelectRemoveMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  Node* a_add_node = FindNode("add", func_a);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_add_node = FindNode("add", func_c);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_add_internal,
                           integration->InsertNode(b_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));

  EXPECT_EQ(integration->function()->node_count(), 10);
  bool inner_mux_added = false;
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * inner_unity,
      integration->UnifyIntegrationNodes(a_add_internal, c_add_internal,
                                         &inner_mux_added));
  EXPECT_TRUE(inner_mux_added);
  EXPECT_EQ(integration->function()->node_count(), 11);
  XLS_ASSERT_OK_AND_ASSIGN(int64 muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 1);

  bool outer_mux_added = false;
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * outer_unity,
      integration->UnifyIntegrationNodes(a_add_internal, b_add_internal,
                                         &outer_mux_added));
  EXPECT_TRUE(outer_mux_added);
  EXPECT_EQ(integration->function()->node_count(), 12);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);

  // Check inner mux.
  EXPECT_EQ(inner_unity->op(), Op::kSel);
  Select* mux = inner_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), a_add_internal);
  EXPECT_THAT(mux->cases(),
              ElementsAre(a_add_internal, a_add_internal, c_add_internal));

  // Check outer mux.
  EXPECT_EQ(outer_unity->op(), Op::kSel);
  mux = outer_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), a_add_internal);
  EXPECT_THAT(mux->cases(),
              ElementsAre(a_add_internal, b_add_internal, a_add_internal));

  // Check inner bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* inner_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(inner_unity));
  EXPECT_THAT(*inner_occupied, ElementsAre(0, 2));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* inner_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(inner_unity));
  EXPECT_THAT(*inner_last_assigned, ElementsAre(0, 2));

  // Check outer bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* outer_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(outer_unity));
  EXPECT_THAT(*outer_occupied, ElementsAre(0, 1));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* outer_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(outer_unity));
  EXPECT_THAT(*outer_last_assigned, ElementsAre(0, 1));

  // Remove inner mux.
  auto inner_deunify_result = integration->DeUnifyIntegrationNodes(inner_unity);
  EXPECT_TRUE(inner_deunify_result.ok());
  EXPECT_EQ(inner_deunify_result.value(), static_cast<Node*>(nullptr));
  EXPECT_EQ(integration->function()->node_count(), 11);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 1);

  // Check outer mux.
  EXPECT_EQ(outer_unity->op(), Op::kSel);
  mux = outer_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), a_add_internal);
  EXPECT_THAT(mux->cases(),
              ElementsAre(a_add_internal, b_add_internal, a_add_internal));

  // Check outer bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(outer_unity));
  EXPECT_THAT(*outer_occupied, ElementsAre(0, 1));
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(outer_unity));
  EXPECT_THAT(*outer_last_assigned, ElementsAre(0, 1));

  // Remove outer mux.
  auto outer_deunify_result = integration->DeUnifyIntegrationNodes(outer_unity);
  EXPECT_TRUE(outer_deunify_result.ok());
  EXPECT_EQ(outer_deunify_result.value(), static_cast<Node*>(nullptr));
  EXPECT_EQ(integration->function()->node_count(), 10);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 0);
}

TEST_F(IntegratorTest, DeUnifyIntegrationGlobalSelectRevertMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_d, func_a->Clone("func_d"));
  Node* a_add_node = FindNode("add", func_a);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_add_node = FindNode("add", func_c);
  Node* d_add_node = FindNode("add", func_d);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c, func_d},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_add_internal,
                           integration->InsertNode(b_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * d_add_internal,
                           integration->InsertNode(d_add_node));

  EXPECT_EQ(integration->function()->node_count(), 13);
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * outer_unity,
      integration->UnifyIntegrationNodes(a_add_internal, c_add_internal));
  bool outer_mux_added = true;
  XLS_ASSERT_OK_AND_ASSIGN(outer_unity,
                           integration->UnifyIntegrationNodes(
                               outer_unity, d_add_internal, &outer_mux_added));
  EXPECT_FALSE(outer_mux_added);
  EXPECT_EQ(integration->function()->node_count(), 14);
  XLS_ASSERT_OK_AND_ASSIGN(int64 muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 1);

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * inner_unity,
      integration->UnifyIntegrationNodes(a_add_internal, d_add_internal));
  bool inner_mux_added = true;
  XLS_ASSERT_OK_AND_ASSIGN(inner_unity,
                           integration->UnifyIntegrationNodes(
                               inner_unity, b_add_internal, &inner_mux_added));
  EXPECT_FALSE(inner_mux_added);
  EXPECT_EQ(integration->function()->node_count(), 15);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);

  // Check inner mux.
  EXPECT_EQ(inner_unity->op(), Op::kSel);
  Select* mux = inner_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, b_add_internal,
                                        a_add_internal, d_add_internal));

  // Check outer mux.
  EXPECT_EQ(outer_unity->op(), Op::kSel);
  mux = outer_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                        c_add_internal, d_add_internal));

  // Check inner bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* inner_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(inner_unity));
  EXPECT_THAT(*inner_occupied, ElementsAre(0, 1, 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* inner_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(inner_unity));
  EXPECT_THAT(*inner_last_assigned, ElementsAre(1));

  // Check outer bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* outer_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(outer_unity));
  EXPECT_THAT(*outer_occupied, ElementsAre(0, 2, 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      const std::set<int64>* outer_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(outer_unity));
  EXPECT_THAT(*outer_last_assigned, ElementsAre(3));

  // Revert inner mux.
  XLS_ASSERT_OK_AND_ASSIGN(inner_unity,
                           integration->DeUnifyIntegrationNodes(inner_unity));
  EXPECT_EQ(integration->function()->node_count(), 15);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);

  // Check inner mux.
  EXPECT_EQ(inner_unity->op(), Op::kSel);
  mux = inner_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                        a_add_internal, d_add_internal));

  // Check outer mux.
  EXPECT_EQ(outer_unity->op(), Op::kSel);
  mux = outer_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                        c_add_internal, d_add_internal));

  // Check inner bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);
  XLS_ASSERT_OK_AND_ASSIGN(
      inner_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(inner_unity));
  EXPECT_THAT(*inner_occupied, ElementsAre(0, 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      inner_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(inner_unity));
  EXPECT_TRUE(inner_last_assigned->empty());

  // Check outer bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(outer_unity));
  EXPECT_THAT(*outer_occupied, ElementsAre(0, 2, 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(outer_unity));
  EXPECT_THAT(*outer_last_assigned, ElementsAre(3));

  // Revert outer mux.
  XLS_ASSERT_OK_AND_ASSIGN(outer_unity,
                           integration->DeUnifyIntegrationNodes(outer_unity));
  EXPECT_EQ(integration->function()->node_count(), 15);
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);

  // Check inner mux.
  EXPECT_EQ(inner_unity->op(), Op::kSel);
  mux = inner_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                        a_add_internal, d_add_internal));

  // Check outer mux.
  EXPECT_EQ(outer_unity->op(), Op::kSel);
  mux = outer_unity->As<Select>();
  EXPECT_EQ(mux->selector(), integration->global_mux_select());
  EXPECT_EQ(mux->default_value(), std::nullopt);
  EXPECT_THAT(mux->cases(), ElementsAre(a_add_internal, a_add_internal,
                                        c_add_internal, a_add_internal));

  // Check inner bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(muxes_tracked,
                           integration->GetNumberOfGlobalMuxesTracked());
  EXPECT_EQ(muxes_tracked, 2);
  XLS_ASSERT_OK_AND_ASSIGN(
      inner_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(inner_unity));
  EXPECT_THAT(*inner_occupied, ElementsAre(0, 3));
  XLS_ASSERT_OK_AND_ASSIGN(
      inner_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(inner_unity));
  EXPECT_TRUE(inner_last_assigned->empty());

  // Check outer bookkeeping.
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_occupied,
      integration->GetGlobalMuxOccupiedCaseIndexes(outer_unity));
  EXPECT_THAT(*outer_occupied, ElementsAre(0, 2));
  XLS_ASSERT_OK_AND_ASSIGN(
      outer_last_assigned,
      integration->GetGlobalMuxLastCaseIndexesAdded(outer_unity));
  EXPECT_TRUE(outer_last_assigned->empty());

  // Can't do repeated reversions.
  auto inner_unity_repeated_revert_result =
      integration->DeUnifyIntegrationNodes(inner_unity);
  EXPECT_FALSE(inner_unity_repeated_revert_result.ok());
  auto outer_unity_repeated_revert_result =
      integration->DeUnifyIntegrationNodes(outer_unity);
  EXPECT_FALSE(outer_unity_repeated_revert_result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationGlobalSelectMuxNotCreatedByUnifyCall) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(in1, in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  Node* a_add_node = FindNode("add", func_a);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_add_node = FindNode("add", func_c);
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c},
          IntegrationOptions().unique_select_signal_per_mux(false))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_internal,
                           integration->InsertNode(a_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_add_internal,
                           integration->InsertNode(b_add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * c_add_internal,
                           integration->InsertNode(c_add_node));

  std::vector<Node*> cases = {a_add_internal, b_add_internal, c_add_internal};
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * new_mux,
      integration->function()->MakeNode<Select>(
          /*loc=*/std::nullopt, integration->global_mux_select(), cases,
          /*default_value=*/a_add_internal));

  // Remove mux. Shouldn't be able to deunify mux not created
  // by a unify call.
  auto deunify_result = integration->DeUnifyIntegrationNodes(new_mux);
  EXPECT_FALSE(deunify_result.ok());
}

TEST_F(IntegratorTest, InsertNodeSimple) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a1 = fb_a.Param("a1", p->GetBitsType(2));
  auto a2 = fb_a.Param("a2", p->GetBitsType(2));
  fb_a.Add(a1, a2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a})));

  Node* a1_node = FindNode("a1", func_a);
  Node* a2_node = FindNode("a2", func_a);
  Node* add_node = func_a->return_value();

  XLS_ASSERT_OK_AND_ASSIGN(Node * integrated_add,
                           integration->InsertNode(add_node));

  EXPECT_TRUE(integration->HasMapping(add_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * add_node_map,
                           integration->GetNodeMapping(add_node));
  EXPECT_EQ(add_node_map, integrated_add);
  EXPECT_TRUE(integration->IntegrationFunctionOwnsNode(integrated_add));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a1_map_target,
                           integration->GetNodeMapping(a1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a2_map_target,
                           integration->GetNodeMapping(a2_node));
  EXPECT_THAT(integrated_add->operands(),
              ElementsAre(a1_map_target, a2_map_target));
}

TEST_F(IntegratorTest, InsertNodeRepeatedly) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a1 = fb_a.Param("a1", p->GetBitsType(2));
  auto a2 = fb_a.Param("a2", p->GetBitsType(2));
  fb_a.Add(a1, a2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a})));
  Node* add_node = func_a->return_value();

  XLS_ASSERT_OK(integration->InsertNode(add_node));
  auto repeat_result = integration->InsertNode(add_node);
  EXPECT_FALSE(repeat_result.ok());
}

TEST_F(IntegratorTest, InsertNodeIntegrationNode) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a1 = fb_a.Param("a1", p->GetBitsType(2));
  auto a2 = fb_a.Param("a2", p->GetBitsType(2));
  fb_a.Add(a1, a2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a})));
  Node* add_node = func_a->return_value();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      integration->function()->MakeNodeWithName<Param>(
          /*loc=*/std::nullopt, "internal_1", p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_cat,
      integration->function()->MakeNode<Concat>(
          /*loc=*/std::nullopt, std::vector<Node*>({internal_1, internal_1})));

  auto unmapped_cost_result = integration->GetInsertNodeCost(internal_cat);
  EXPECT_FALSE(unmapped_cost_result.ok());
  auto unmapped_merge_result = integration->InsertNode(internal_cat);
  EXPECT_FALSE(unmapped_merge_result.ok());

  XLS_ASSERT_OK_AND_ASSIGN(Node * map_target,
                           integration->InsertNode(add_node));
  auto mapped_cost_result = integration->GetInsertNodeCost(map_target);
  EXPECT_FALSE(mapped_cost_result.ok());
  auto mapped_merge_result = integration->InsertNode(map_target);
  EXPECT_FALSE(mapped_merge_result.ok());
}

TEST_F(IntegratorTest, GetNodeCost) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a1 = fb_a.Param("a1", p->GetBitsType(2));
  auto a2 = fb_a.Param("a2", p->GetBitsType(2));
  fb_a.Add(a1, a2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a})));

  Node* add_node = func_a->return_value();
  XLS_ASSERT_OK_AND_ASSIGN(float cost,
                           integration->GetInsertNodeCost(add_node));
  EXPECT_EQ(cost, integration->GetNodeCost(add_node));
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesExternalNode) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function external_func("external", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_1,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_2,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "external_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_sel,
      external_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt,
                                            "external_sel", p->GetBitsType(1)));
  std::vector<Node*> elements = {external_1, external_2};
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * external_mux,
      external_func.MakeNode<Select>(/*loc=*/std::nullopt, external_sel,
                                     elements, /*default_value=*/std::nullopt));

  // Can't DeUnify external node.
  auto result = integration->DeUnifyIntegrationNodes(external_mux);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesNonUnifyNonMux) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_cat,
      internal_func.MakeNode<Concat>(
          /*loc=*/std::nullopt, std::vector<Node*>({internal_1, internal_1})));

  auto result = integration->DeUnifyIntegrationNodes(internal_cat);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesNonUnifyMuxDoesNotHaveTwoCases) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_sel,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt,
                                            "internal_sel", p->GetBitsType(1)));
  std::vector<Node*> elements = {internal_1};
  XLS_ASSERT_OK_AND_ASSIGN(Node * internal_mux,
                           integration->function()->MakeNode<Select>(
                               /*loc=*/std::nullopt, internal_sel, elements,
                               /*default_value=*/internal_sel));

  auto result = integration->DeUnifyIntegrationNodes(internal_mux);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesNonUnifyMuxSameOperands) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_sel,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt,
                                            "internal_sel", p->GetBitsType(1)));
  XLS_ASSERT_OK(integration->UnifyIntegrationNodes(internal_1, internal_2));
  std::vector<Node*> elements = {internal_1, internal_2};
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_mux,
      internal_func.MakeNode<Select>(/*loc=*/std::nullopt, internal_sel,
                                     elements, /*default_value=*/std::nullopt));

  auto result = integration->DeUnifyIntegrationNodes(internal_mux);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesNonUnifyMuxDifferentOperands) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_3,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_3",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_4,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_4",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_sel,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt,
                                            "internal_sel", p->GetBitsType(1)));
  XLS_ASSERT_OK(integration->UnifyIntegrationNodes(internal_1, internal_2));
  std::vector<Node*> elements = {internal_3, internal_4};
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_mux,
      internal_func.MakeNode<Select>(/*loc=*/std::nullopt, internal_sel,
                                     elements, /*default_value=*/std::nullopt));

  auto result = integration->DeUnifyIntegrationNodes(internal_mux);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesMuxHasUsers) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(Node * unify_mux, integration->UnifyIntegrationNodes(
                                                 internal_1, internal_2));
  XLS_ASSERT_OK(internal_func.MakeNode<Concat>(
      /*loc=*/std::nullopt, std::vector<Node*>({unify_mux, unify_mux})));

  auto result = integration->DeUnifyIntegrationNodes(unify_mux);
  EXPECT_FALSE(result.ok());
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesRemoveParam) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(Node * unify_mux, integration->UnifyIntegrationNodes(
                                                 internal_1, internal_2));
  Node* sel = unify_mux->As<Select>()->selector();

  EXPECT_EQ(integration->function()->node_count(), 4);
  auto result = integration->DeUnifyIntegrationNodes(unify_mux);

  auto integration_contains = [&integration](Node* target_node) {
    for (Node* node : integration->function()->nodes()) {
      if (target_node == node) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.value(), static_cast<Node*>(nullptr));
  EXPECT_EQ(integration->function()->node_count(), 2);
  EXPECT_FALSE(integration_contains(unify_mux));
  EXPECT_FALSE(integration_contains(sel));

  bool added_mux = false;
  XLS_ASSERT_OK(
      integration->UnifyIntegrationNodes(internal_1, internal_2, &added_mux));
  EXPECT_TRUE(added_mux);
}

TEST_F(IntegratorTest, DeUnifyIntegrationNodesKeepParam) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  Function& internal_func = *integration->function();

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_1",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      internal_func.MakeNodeWithName<Param>(/*loc=*/std::nullopt, "internal_2",
                                            p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(Node * unify_mux, integration->UnifyIntegrationNodes(
                                                 internal_1, internal_2));

  Node* sel = unify_mux->As<Select>()->selector();
  XLS_ASSERT_OK(internal_func.MakeNode<Concat>(/*loc=*/std::nullopt,
                                               std::vector<Node*>({sel, sel})));

  EXPECT_EQ(integration->function()->node_count(), 5);
  auto result = integration->DeUnifyIntegrationNodes(unify_mux);

  auto integration_contains = [&integration](Node* target_node) {
    for (Node* node : integration->function()->nodes()) {
      if (target_node == node) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.value(), static_cast<Node*>(nullptr));
  EXPECT_EQ(integration->function()->node_count(), 4);
  EXPECT_FALSE(integration_contains(unify_mux));
  EXPECT_TRUE(integration_contains(sel));
}

TEST_F(IntegratorTest, UnifyNodeOperands) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  fb.Add(in1, in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("clone"));
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_add = a_in1->users().at(0);
  Node* b_in1 = FindNode("in1", func_b);
  Node* b_in2 = FindNode("in2", func_b);
  Node* b_add = b_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_map, integration->InsertNode(a_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_map, integration->InsertNode(a_in2));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1, a_in1_map));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_map, integration->InsertNode(b_in2));

  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add, b_add, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 1);
  Select* mux = added_muxes.at(0)->As<Select>();
  EXPECT_THAT(mux->cases(), ElementsAre(a_in2_map, b_in2_map));
  EXPECT_THAT(unified_operands, ElementsAre(a_in1_map, mux));
}

TEST_F(IntegratorTest, UnifyNodeOperandsMultipleMux) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  fb.Add(in1, in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("clone"));
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_add = a_in1->users().at(0);
  Node* b_in1 = FindNode("in1", func_b);
  Node* b_in2 = FindNode("in2", func_b);
  Node* b_add = b_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_map, integration->InsertNode(a_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_map, integration->InsertNode(a_in2));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in1_map, integration->InsertNode(b_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_map, integration->InsertNode(b_in2));

  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add, b_add, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 2);
  Select* mux1 = added_muxes.at(0)->As<Select>();
  EXPECT_THAT(mux1->cases(), ElementsAre(a_in1_map, b_in1_map));
  Select* mux2 = added_muxes.at(1)->As<Select>();
  EXPECT_THAT(mux2->cases(), ElementsAre(a_in2_map, b_in2_map));
  EXPECT_THAT(unified_operands, ElementsAre(mux1, mux2));
}

TEST_F(IntegratorTest, UnifyNodeOperandsReapeatedMux) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  fb.Add(in1, in1);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("clone"));
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_add = a_in1->users().at(0);
  Node* b_in1 = FindNode("in1", func_b);
  Node* b_add = b_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_map, integration->InsertNode(a_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in1_map, integration->InsertNode(b_in1));

  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add, b_add, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 1);
  Select* mux = added_muxes.at(0)->As<Select>();
  EXPECT_THAT(mux->cases(), ElementsAre(a_in1_map, b_in1_map));
  EXPECT_THAT(unified_operands, ElementsAre(mux, mux));
}

TEST_F(IntegratorTest, UnifyNodeOperandsNoAddedMuxesArg) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  fb.Add(in1, in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("clone"));
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_add = a_in1->users().at(0);
  Node* b_in1 = FindNode("in1", func_b);
  Node* b_in2 = FindNode("in2", func_b);
  Node* b_add = b_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_map, integration->InsertNode(a_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_map, integration->InsertNode(a_in2));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1, a_in1_map));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in2, a_in2_map));

  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add, b_add, &added_muxes));
  EXPECT_EQ(a_in2_map->users().size(), 0);
}

TEST_F(IntegratorTest, MergeBackendErrorNonMappedInternal) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  fb.Add(in1, in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_add = a_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target, integration->InsertNode(a_in1));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target, integration->InsertNode(a_in2));
  XLS_ASSERT_OK_AND_ASSIGN(Node * internal_add,
                           a_add->CloneInNewFunction(
                               std::vector<Node*>({a_in1_target, a_in2_target}),
                               integration->function()));

  auto result1 = integration->GetMergeNodesCost(a_add, internal_add);
  EXPECT_FALSE(result1.ok());
  auto result2 = integration->GetMergeNodesCost(internal_add, a_add);
  EXPECT_FALSE(result2.ok());
}

TEST_F(IntegratorTest, MergeBackendErrorMappedExternal) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  fb.Add(in1, in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("clone"));
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_add = a_in1->users().at(0);
  Node* b_in1 = FindNode("in1", func_b);
  Node* b_in2 = FindNode("in2", func_b);
  Node* b_add = b_in1->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK(integration->InsertNode(a_in1));
  XLS_ASSERT_OK(integration->InsertNode(a_in2));
  XLS_ASSERT_OK(integration->InsertNode(a_add));
  XLS_ASSERT_OK(integration->InsertNode(b_in1));
  XLS_ASSERT_OK(integration->InsertNode(b_in2));

  auto result1 = integration->GetMergeNodesCost(a_add, b_add);
  EXPECT_FALSE(result1.ok());
  auto result2 = integration->GetMergeNodesCost(b_add, a_add);
  EXPECT_FALSE(result2.ok());
}

TEST_F(IntegratorTest, MergeBackendErrorNodesFromSameFunction) {
  auto p = CreatePackage();
  FunctionBuilder fb("func", p.get());
  auto in1 = fb.Param("in1", p->GetBitsType(2));
  auto in2 = fb.Param("in2", p->GetBitsType(2));
  auto in3 = fb.Param("in3", p->GetBitsType(2));
  fb.Add(in1, in2);
  fb.Add(in1, in3);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb.Build());
  Node* a_in1 = FindNode("in1", func_a);
  Node* a_in2 = FindNode("in2", func_a);
  Node* a_in3 = FindNode("in3", func_a);
  Node* a_add1 = a_in1->users().at(0);
  Node* a_add2 = a_in3->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK(integration->InsertNode(a_in1));
  XLS_ASSERT_OK(integration->InsertNode(a_in2));
  XLS_ASSERT_OK(integration->InsertNode(a_in3));

  auto result1 = integration->GetMergeNodesCost(a_add1, a_add2);
  EXPECT_FALSE(result1.ok());
}

TEST_F(IntegratorTest, MergeBackendDoNotMergeIncompatible) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2);
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("in2", p->GetBitsType(2));
  fb_b.Concat({b_in1, b_in2});
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("in1", func_a);
  Node* a_in2_node = FindNode("in2", func_a);
  Node* a_add_node = a_in1_node->users().at(0);
  Node* b_in1_node = FindNode("in1", func_b);
  Node* b_in2_node = FindNode("in2", func_b);
  Node* b_cat_node = b_in1_node->users().at(0);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK(integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK(integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK(integration->InsertNode(b_in1_node));
  XLS_ASSERT_OK(integration->InsertNode(b_in2_node));

  // Cost frontend.
  XLS_ASSERT_OK_AND_ASSIGN(auto optional_cost, integration->GetMergeNodesCost(
                                                   a_add_node, b_cat_node));
  EXPECT_FALSE(optional_cost.has_value());

  // Merge frontend.
  EXPECT_FALSE(integration->MergeNodes(a_add_node, b_cat_node).ok());
}

TEST_F(IntegratorTest, MergeCostExternalExternaTwoMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("in2", p->GetBitsType(2));
  auto b_sel = fb_b.Param("sel", p->GetBitsType(1));
  fb_b.Add(b_in1, b_in2,
           /*loc=*/absl::nullopt, "add");
  fb_b.Select(b_sel, {b_in1, b_in2},
              /*default_value=*/absl::nullopt,
              /*loc=*/absl::nullopt, "mux");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("in1", func_a);
  Node* a_in2_node = FindNode("in2", func_a);
  Node* a_add_node = FindNode("add", func_a);
  Node* b_in1_node = FindNode("in1", func_b);
  Node* b_in2_node = FindNode("in2", func_b);
  Node* b_add_node = FindNode("add", func_b);
  Node* ref_mux_node = FindNode("mux", func_b);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target,
                           integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target,
                           integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in1_target,
                           integration->InsertNode(b_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_target,
                           integration->InsertNode(b_in2_node));

  // Get cost.
  XLS_ASSERT_OK_AND_ASSIGN(auto optional_cost, integration->GetMergeNodesCost(
                                                   a_add_node, b_add_node));
  XLS_ASSERT_OK(VerifyFunction(integration->function()));

  // Check cost.
  EXPECT_TRUE(optional_cost.has_value());
  float expected_cost = integration->GetNodeCost(a_add_node) +
                        2 * integration->GetNodeCost(ref_mux_node);
  EXPECT_FLOAT_EQ(optional_cost.value(), expected_cost);

  // Reverse order of merged nodes.
  XLS_ASSERT_OK_AND_ASSIGN(
      optional_cost, integration->GetMergeNodesCost(b_add_node, a_add_node));
  EXPECT_TRUE(optional_cost.has_value());
  EXPECT_FLOAT_EQ(optional_cost.value(), expected_cost);

  // Check function un-altered and mappings preserved.
  EXPECT_EQ(integration->function()->node_count(), 4);
  absl::flat_hash_set<Node*> found_nodes;
  for (auto* node : integration->function()->nodes()) {
    found_nodes.insert(node);
  }
  auto check_param = [&](const Node* src, const Node* target) {
    XLS_ASSERT_OK_AND_ASSIGN(Node * observed_target,
                             integration->GetNodeMapping(src));
    EXPECT_EQ(target, observed_target);
    EXPECT_TRUE(found_nodes.contains(target));
    EXPECT_TRUE(target->users().empty());
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes_map_to_target,
                             integration->GetNodesMappedToNode(target));
    EXPECT_EQ(nodes_map_to_target->size(), 1);
    EXPECT_TRUE(nodes_map_to_target->contains(src));
  };
  check_param(a_in1_node, a_in1_target);
  check_param(a_in2_node, a_in2_target);
  check_param(b_in1_node, b_in1_target);
  check_param(b_in2_node, b_in2_target);

  // Check that mux tracking was not updated.
  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add_node, b_add_node, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 2);
}

TEST_F(IntegratorTest, MergeCostInternalExternalOneMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("in2", p->GetBitsType(2));
  auto b_sel = fb_b.Param("sel", p->GetBitsType(1));
  fb_b.Add(b_in1, b_in2,
           /*loc=*/absl::nullopt, "add");
  fb_b.Select(b_sel, {b_in1, b_in2},
              /*default_value=*/absl::nullopt,
              /*loc=*/absl::nullopt, "mux");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("in1", func_a);
  Node* a_in2_node = FindNode("in2", func_a);
  Node* a_add_node = FindNode("add", func_a);
  Node* b_in1_node = FindNode("in1", func_b);
  Node* b_in2_node = FindNode("in2", func_b);
  Node* b_add_node = FindNode("add", func_b);
  Node* ref_mux_node = FindNode("mux", func_b);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target,
                           integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target,
                           integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1_node, a_in1_target));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_target,
                           integration->InsertNode(b_in2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_target,
                           integration->InsertNode(a_add_node));

  // Get cost.
  XLS_ASSERT_OK_AND_ASSIGN(auto optional_cost, integration->GetMergeNodesCost(
                                                   a_add_target, b_add_node));
  XLS_ASSERT_OK(VerifyFunction(integration->function()));

  // Check cost.
  EXPECT_TRUE(optional_cost.has_value());
  float expected_cost = integration->GetNodeCost(ref_mux_node);
  EXPECT_FLOAT_EQ(optional_cost.value(), expected_cost);

  // Reverse order of merged nodes.
  XLS_ASSERT_OK_AND_ASSIGN(
      optional_cost, integration->GetMergeNodesCost(b_add_node, a_add_target));
  EXPECT_TRUE(optional_cost.has_value());
  EXPECT_FLOAT_EQ(optional_cost.value(), expected_cost);

  // Check function un-altered and mappings preserved.
  EXPECT_EQ(integration->function()->node_count(), 4);
  absl::flat_hash_set<Node*> found_nodes;
  for (auto* node : integration->function()->nodes()) {
    found_nodes.insert(node);
  }
  auto check_param = [&](std::vector<const Node*> srcs, const Node* target,
                         bool used_by_add) {
    EXPECT_TRUE(found_nodes.contains(target));
    if (used_by_add) {
      EXPECT_EQ(target->users().size(), 1);
      EXPECT_EQ(target->users().front(), a_add_target);
    } else {
      EXPECT_TRUE(target->users().empty());
    }
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes_map_to_target,
                             integration->GetNodesMappedToNode(target));
    for (auto src : srcs) {
      XLS_ASSERT_OK_AND_ASSIGN(Node * observed_target,
                               integration->GetNodeMapping(src));
      EXPECT_EQ(target, observed_target);
      EXPECT_TRUE(nodes_map_to_target->contains(src));
    }
    EXPECT_EQ(nodes_map_to_target->size(), srcs.size());
  };
  check_param({a_in1_node, b_in1_node}, a_in1_target, true);
  check_param({a_in2_node}, a_in2_target, true);
  check_param({b_in2_node}, b_in2_target, false);
  EXPECT_TRUE(found_nodes.contains(a_add_target));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_observed_target,
                           integration->GetNodeMapping(a_add_node));
  EXPECT_EQ(a_add_observed_target, a_add_target);
  XLS_ASSERT_OK_AND_ASSIGN(auto nodes_mapped_to_add,
                           integration->GetNodesMappedToNode(a_add_target));
  EXPECT_EQ(nodes_mapped_to_add->size(), 1);
  EXPECT_TRUE(nodes_mapped_to_add->contains(a_add_node));

  // Check that mux tracking was not updated.
  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add_node, b_add_node, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 1);
}

TEST_F(IntegratorTest, MergeNodesExternalExternaTwoMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("a_in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("a_in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2,
           /*loc=*/absl::nullopt, "a_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("b_in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("b_in2", p->GetBitsType(2));
  fb_b.Add(b_in1, b_in2,
           /*loc=*/absl::nullopt, "b_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("a_in1", func_a);
  Node* a_in2_node = FindNode("a_in2", func_a);
  Node* a_add_node = FindNode("a_add", func_a);
  Node* b_in1_node = FindNode("b_in1", func_b);
  Node* b_in2_node = FindNode("b_in2", func_b);
  Node* b_add_node = FindNode("b_add", func_b);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target,
                           integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target,
                           integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in1_target,
                           integration->InsertNode(b_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_target,
                           integration->InsertNode(b_in2_node));

  // Merge.
  XLS_ASSERT_OK_AND_ASSIGN(auto generated_nodes,
                           integration->MergeNodes(a_add_node, b_add_node));
  XLS_ASSERT_OK(VerifyFunction(integration->function()));

  // Check merge.
  EXPECT_EQ(integration->function()->node_count(), 9);
  EXPECT_EQ(generated_nodes.size(), 1);
  Node* add_target = generated_nodes.front();
  EXPECT_THAT(add_target,
              m::Add(m::Select(m::Param("a_in1_b_in1_mux_sel"),
                               {m::Param("a_in1"), m::Param("b_in1")}),
                     m::Select(m::Param("a_in2_b_in2_mux_sel"),
                               {m::Param("a_in2"), m::Param("b_in2")})));

  // Check mapping.
  auto check_mapping = [&](std::vector<const Node*> srcs, const Node* target) {
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes_map_to_target,
                             integration->GetNodesMappedToNode(target));
    EXPECT_EQ(nodes_map_to_target->size(), srcs.size());
    for (auto src : srcs) {
      XLS_ASSERT_OK_AND_ASSIGN(Node * observed_target,
                               integration->GetNodeMapping(src));
      EXPECT_EQ(target, observed_target);
      EXPECT_TRUE(nodes_map_to_target->contains(src));
    }
  };
  check_mapping({a_in1_node}, a_in1_target);
  check_mapping({a_in2_node}, a_in2_target);
  check_mapping({b_in1_node}, b_in1_target);
  check_mapping({b_in2_node}, b_in2_target);
  check_mapping({a_add_node, b_add_node}, add_target);

  // Check that mux tracking was updated.
  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add_node, b_add_node, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 0);
}

TEST_F(IntegratorTest, MergeNodesInternalExternalOneMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("a_in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("a_in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2,
           /*loc=*/absl::nullopt, "a_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("b_in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("b_in2", p->GetBitsType(2));
  fb_b.Add(b_in1, b_in2,
           /*loc=*/absl::nullopt, "b_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("a_in1", func_a);
  Node* a_in2_node = FindNode("a_in2", func_a);
  Node* a_add_node = FindNode("a_add", func_a);
  Node* b_in1_node = FindNode("b_in1", func_b);
  Node* b_in2_node = FindNode("b_in2", func_b);
  Node* b_add_node = FindNode("b_add", func_b);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target,
                           integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target,
                           integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1_node, a_in1_target));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_target,
                           integration->InsertNode(b_in2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_initial_target,
                           integration->InsertNode(a_add_node));

  // Merge.
  XLS_ASSERT_OK_AND_ASSIGN(
      auto generated_nodes,
      integration->MergeNodes(a_add_initial_target, b_add_node));
  XLS_ASSERT_OK(VerifyFunction(integration->function()));

  // Check merge.
  EXPECT_EQ(integration->function()->node_count(), 6);
  EXPECT_EQ(generated_nodes.size(), 1);
  Node* add_target = generated_nodes.front();
  EXPECT_THAT(add_target,
              m::Add(m::Param("a_in1"),
                     m::Select(m::Param("a_in2_b_in2_mux_sel"),
                               {m::Param("a_in2"), m::Param("b_in2")})));

  // Check mapping.
  auto check_mapping = [&](std::vector<const Node*> srcs, const Node* target) {
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes_map_to_target,
                             integration->GetNodesMappedToNode(target));
    EXPECT_EQ(nodes_map_to_target->size(), srcs.size());
    for (auto src : srcs) {
      XLS_ASSERT_OK_AND_ASSIGN(Node * observed_target,
                               integration->GetNodeMapping(src));
      EXPECT_EQ(target, observed_target);
      EXPECT_TRUE(nodes_map_to_target->contains(src));
    }
  };
  check_mapping({a_in1_node, b_in1_node}, a_in1_target);
  check_mapping({a_in2_node}, a_in2_target);
  check_mapping({b_in2_node}, b_in2_target);
  check_mapping({a_add_node, b_add_node}, add_target);
  EXPECT_FALSE(integration->HasMapping(a_add_initial_target));
  EXPECT_FALSE(integration->IsMappingTarget(a_add_initial_target));

  // Check that mux tracking was updated.
  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(a_add_node, b_add_node, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 0);
}

TEST_F(IntegratorTest, MergeNodesExternalInternalOneMux) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("a_in1", p->GetBitsType(2));
  auto a_in2 = fb_a.Param("a_in2", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in2,
           /*loc=*/absl::nullopt, "a_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  FunctionBuilder fb_b("func_b", p.get());
  auto b_in1 = fb_b.Param("b_in1", p->GetBitsType(2));
  auto b_in2 = fb_b.Param("b_in2", p->GetBitsType(2));
  fb_b.Add(b_in1, b_in2,
           /*loc=*/absl::nullopt, "b_add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, fb_b.Build());
  Node* a_in1_node = FindNode("a_in1", func_a);
  Node* a_in2_node = FindNode("a_in2", func_a);
  Node* a_add_node = FindNode("a_add", func_a);
  Node* b_in1_node = FindNode("b_in1", func_b);
  Node* b_in2_node = FindNode("b_in2", func_b);
  Node* b_add_node = FindNode("b_add", func_b);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {},
          IntegrationOptions().unique_select_signal_per_mux(true))));

  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in1_target,
                           integration->InsertNode(a_in1_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_in2_target,
                           integration->InsertNode(a_in2_node));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1_node, a_in1_target));
  XLS_ASSERT_OK_AND_ASSIGN(Node * b_in2_target,
                           integration->InsertNode(b_in2_node));
  XLS_ASSERT_OK_AND_ASSIGN(Node * a_add_initial_target,
                           integration->InsertNode(a_add_node));

  // Merge.
  XLS_ASSERT_OK_AND_ASSIGN(
      auto generated_nodes,
      integration->MergeNodes(b_add_node, a_add_initial_target));
  XLS_ASSERT_OK(VerifyFunction(integration->function()));

  // Check merge.
  EXPECT_EQ(integration->function()->node_count(), 6);
  EXPECT_EQ(generated_nodes.size(), 1);
  Node* add_target = generated_nodes.front();
  EXPECT_THAT(add_target,
              m::Add(m::Param("a_in1"),
                     m::Select(m::Param("b_in2_a_in2_mux_sel"),
                               {m::Param("b_in2"), m::Param("a_in2")})));

  // Check mapping.
  auto check_mapping = [&](std::vector<const Node*> srcs, const Node* target) {
    XLS_ASSERT_OK_AND_ASSIGN(auto nodes_map_to_target,
                             integration->GetNodesMappedToNode(target));
    EXPECT_EQ(nodes_map_to_target->size(), srcs.size());
    for (auto src : srcs) {
      XLS_ASSERT_OK_AND_ASSIGN(Node * observed_target,
                               integration->GetNodeMapping(src));
      EXPECT_EQ(target, observed_target);
      EXPECT_TRUE(nodes_map_to_target->contains(src));
    }
  };
  check_mapping({a_in1_node, b_in1_node}, a_in1_target);
  check_mapping({a_in2_node}, a_in2_target);
  check_mapping({b_in2_node}, b_in2_target);
  check_mapping({a_add_node, b_add_node}, add_target);
  EXPECT_FALSE(integration->HasMapping(a_add_initial_target));
  EXPECT_FALSE(integration->IsMappingTarget(a_add_initial_target));

  // Check that mux tracking was updated.
  std::vector<Node*> added_muxes;
  XLS_ASSERT_OK_AND_ASSIGN(
      std::vector<Node*> unified_operands,
      integration->UnifyNodeOperands(b_add_node, a_add_node, &added_muxes));
  EXPECT_EQ(added_muxes.size(), 0);
}

TEST_F(IntegratorTest, GetSourceFunctionIndexOfNodeTest) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  auto a_in1 = fb_a.Param("in1", p->GetBitsType(2));
  fb_a.Add(a_in1, a_in1,
           /*loc=*/absl::nullopt, "add");
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_d, func_a->Clone("func_d"));
  Node* a_in1_node = FindNode("in1", func_a);
  Node* a_add_node = FindNode("add", func_a);
  Node* b_in1_node = FindNode("in1", func_b);
  Node* b_add_node = FindNode("add", func_b);
  Node* c_in1_node = FindNode("in1", func_c);
  Node* c_add_node = FindNode("add", func_c);
  Node* d_in1_node = FindNode("in1", func_d);
  Node* d_add_node = FindNode("add", func_d);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c})));

  auto test_idx = [&integration](Node* node, int64 expected_index) {
    XLS_ASSERT_OK_AND_ASSIGN(int64 idx,
                             integration->GetSourceFunctionIndexOfNode(node));
    EXPECT_EQ(idx, expected_index);
  };

  test_idx(a_in1_node, 0);
  test_idx(a_add_node, 0);
  test_idx(b_in1_node, 1);
  test_idx(b_add_node, 1);
  test_idx(c_in1_node, 2);
  test_idx(c_add_node, 2);
  auto non_source_result =
      integration->GetSourceFunctionIndexOfNode(d_in1_node);
  EXPECT_FALSE(non_source_result.ok());
  non_source_result = integration->GetSourceFunctionIndexOfNode(d_add_node);
  EXPECT_FALSE(non_source_result.ok());
}

TEST_F(IntegratorTest, GetSourceFunctionIndexesOfNodesMappedToNodeTest) {
  auto p = CreatePackage();
  FunctionBuilder fb_a("func_a", p.get());
  fb_a.Param("in1", p->GetBitsType(2));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_a, fb_a.Build());
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_b, func_a->Clone("func_b"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_c, func_a->Clone("func_c"));
  XLS_ASSERT_OK_AND_ASSIGN(Function * func_d, func_a->Clone("func_d"));
  Node* a_in1_node = FindNode("in1", func_a);
  Node* b_in1_node = FindNode("in1", func_b);
  Node* c_in1_node = FindNode("in1", func_c);
  Node* d_in1_node = FindNode("in1", func_d);

  XLS_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<IntegrationFunction> integration,
      std::move(IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
          p.get(), {func_a, func_b, func_c, func_d})));

  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_1,
      integration->function()->MakeNodeWithName<Param>(
          /*loc=*/std::nullopt, "internal_1", p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_2,
      integration->function()->MakeNodeWithName<Param>(
          /*loc=*/std::nullopt, "internal_2", p->GetBitsType(2)));
  XLS_ASSERT_OK_AND_ASSIGN(
      Node * internal_3,
      integration->function()->MakeNodeWithName<Param>(
          /*loc=*/std::nullopt, "internal_3", p->GetBitsType(2)));

  XLS_ASSERT_OK(integration->SetNodeMapping(a_in1_node, internal_1));
  XLS_ASSERT_OK(integration->SetNodeMapping(c_in1_node, internal_1));
  XLS_ASSERT_OK(integration->SetNodeMapping(b_in1_node, internal_2));
  XLS_ASSERT_OK(integration->SetNodeMapping(d_in1_node, internal_2));

  XLS_ASSERT_OK_AND_ASSIGN(
      std::set<int64> internal_1_source_indexes,
      integration->GetSourceFunctionIndexesOfNodesMappedToNode(internal_1));
  XLS_ASSERT_OK_AND_ASSIGN(
      std::set<int64> internal_2_source_indexes,
      integration->GetSourceFunctionIndexesOfNodesMappedToNode(internal_2));
  EXPECT_THAT(internal_1_source_indexes, ElementsAre(0, 2));
  EXPECT_THAT(internal_2_source_indexes, ElementsAre(1, 3));

  auto non_map_target_result =
      integration->GetSourceFunctionIndexesOfNodesMappedToNode(internal_3);
  EXPECT_FALSE(non_map_target_result.ok());
}

}  // namespace
}  // namespace xls
