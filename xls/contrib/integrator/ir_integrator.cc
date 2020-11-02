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
// limitations under the License

#include "xls/contrib/integrator/ir_integrator.h"

#include "xls/ir/ir_parser.h"

namespace xls {

absl::StatusOr<std::unique_ptr<IntegrationFunction>>
IntegrationFunction::MakeIntegrationFunctionWithParamTuples(
    Package* package, absl::Span<const Function* const> source_functions,
    std::string function_name) {
  // Create integration function object.
  auto integration_function = absl::make_unique<IntegrationFunction>();
  integration_function->package_ = package;

  // Create ir function.
  integration_function->function_ =
      absl::make_unique<Function>(function_name, package);

  // Package source function parameters as tuple parameters to integration
  // function.
  for (const auto* source_func : source_functions) {
    // Add tuple parameter for source function.
    std::vector<Type*> arg_types;
    for (const Node* param : source_func->params()) {
      arg_types.push_back(param->GetType());
    }
    Type* args_tuple_type = package->GetTupleType(arg_types);
    std::string tuple_name = source_func->name() + std::string("ParamTuple");
    XLS_ASSIGN_OR_RETURN(
        Node * args_tuple,
        integration_function->function_->MakeNodeWithName<Param>(
            /*loc=*/std::nullopt, tuple_name, args_tuple_type));

    // Add TupleIndex nodes inside function to unpack tuple parameter.
    int64 parameter_index = 0;
    for (const Node* param : source_func->params()) {
      XLS_ASSIGN_OR_RETURN(
          Node * tuple_index,
          integration_function->function_->MakeNode<TupleIndex>(
              /*loc=*/std::nullopt, args_tuple, parameter_index));
      XLS_RETURN_IF_ERROR(
          integration_function->SetNodeMapping(param, tuple_index));
      parameter_index++;
    }
  }

  return std::move(integration_function);
}

absl::Status IntegrationFunction::SetNodeMapping(const Node* source,
                                                 Node* map_target) {
  // Validate map pairing.
  XLS_RET_CHECK_NE(source, map_target);
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(map_target));
  XLS_RET_CHECK(
      !(IntegrationFunctionOwnsNode(source) && !IsMappingTarget(source)));

  // 'original' is itself a member of the integrated function.
  if (IntegrationFunctionOwnsNode(source)) {
    absl::flat_hash_set<const Node*>& nodes_that_map_to_source =
        integrated_node_to_original_nodes_map_.at(source);

    // Nodes that previously mapped to original now map to map_target.
    for (const Node* original_node : nodes_that_map_to_source) {
      integrated_node_to_original_nodes_map_[map_target].insert(original_node);
      XLS_RET_CHECK(HasMapping(original_node));
      original_node_to_integrated_node_map_[original_node] = map_target;
    }

    // No nodes map to source anymore.
    integrated_node_to_original_nodes_map_.erase(source);

    // 'source' is an external node.
  } else {
    original_node_to_integrated_node_map_[source] = map_target;
    integrated_node_to_original_nodes_map_[map_target].insert(source);
  }

  return absl::OkStatus();
}

absl::StatusOr<Node*> IntegrationFunction::GetNodeMapping(
    const Node* original) const {
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(original));
  if (!HasMapping(original)) {
    return absl::InternalError("No mapping found for original node");
  }
  return original_node_to_integrated_node_map_.at(original);
}

absl::StatusOr<const absl::flat_hash_set<const Node*>*>
IntegrationFunction::GetNodesMappedToNode(const Node* map_target) const {
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(map_target));
  if (!IsMappingTarget(map_target)) {
    return absl::InternalError("No mappings found for map target node");
  }
  return &integrated_node_to_original_nodes_map_.at(map_target);
}

absl::StatusOr<std::vector<Node*>> IntegrationFunction::GetIntegratedOperands(
    const Node* node) const {
  std::vector<Node*> operand_mappings;
  operand_mappings.reserve(node->operands().size());

  if (IntegrationFunctionOwnsNode(node)) {
    operand_mappings.insert(operand_mappings.end(), node->operands().begin(),
                            node->operands().end());
    return operand_mappings;
  }

  for (const auto* operand : node->operands()) {
    if (!HasMapping(operand)) {
      // TODO(jbaileyhandle): Implement.
      return absl::UnimplementedError(
          "GetIntegratedOperands for unmapped operands not yet implemented");
    } else {
      XLS_ASSIGN_OR_RETURN(Node * operand_map_target, GetNodeMapping(operand));
      operand_mappings.push_back(operand_map_target);
    }
  }
  return operand_mappings;
}

absl::StatusOr<Node*> IntegrationFunction::UnifyIntegrationNodes(
    Node* node_a, Node* node_b, bool* new_mux_added) {
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(node_a));
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(node_b));
  XLS_RET_CHECK_EQ(node_a->GetType(), node_b->GetType());
  if (new_mux_added != nullptr) {
    *new_mux_added = false;
  }

  // Same node.
  if (node_a == node_b) {
    return node_a;
  }

  // Already created a mux to select between these nodes.
  // Note that we search for (node_a, node_b) but not for
  // (node_b, node_a) because a reversed pair implies
  // an inverted select signal.
  // TODO(jbaileyhandle): Create a canonical ordering among nodes to avoid
  // this issue.
  std::pair<const Node*, const Node*> key = std::make_pair(node_a, node_b);
  if (node_pair_to_mux_.contains(key)) {
    return node_pair_to_mux_.at(key);
  }

  // Create a new mux.
  // TODO(jbaileyhandle): Currently create a new select line per mux
  // to maximize programmability. May want to have a single set
  // of select bits for the integrated function such that we can
  // only configure to one of the input functions.
  std::string select_name =
      node_a->GetName() + "_" + node_b->GetName() + "_mux_sel";
  XLS_ASSIGN_OR_RETURN(Node * select, function_->MakeNodeWithName<Param>(
                                          /*loc=*/std::nullopt, select_name,
                                          package_->GetBitsType(1)));
  std::vector<Node*> elements = {node_a, node_b};
  XLS_ASSIGN_OR_RETURN(Node * mux, function_->MakeNode<Select>(
                                       /*loc=*/std::nullopt, select, elements,
                                       /*default_value=*/std::nullopt));

  node_pair_to_mux_[key] = mux;
  if (new_mux_added != nullptr) {
    *new_mux_added = true;
  }
  return mux;
}

absl::StatusOr<std::vector<Node*>> IntegrationFunction::UnifyNodeOperands(
    const Node* node_a, const Node* node_b, std::vector<Node*>* added_muxes) {
  XLS_RET_CHECK_EQ(node_a->operands().size(), node_b->operands().size());

  // Get mapped operands.
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> a_ops, GetIntegratedOperands(node_a));
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> b_ops, GetIntegratedOperands(node_b));

  // Unify operands.
  std::vector<Node*> unified_operands;
  unified_operands.reserve(a_ops.size());
  for (int64 idx = 0; idx < a_ops.size(); ++idx) {
    bool mux_added;
    XLS_ASSIGN_OR_RETURN(
        Node * uni_op,
        UnifyIntegrationNodes(a_ops.at(idx), b_ops.at(idx), &mux_added));
    unified_operands.push_back(uni_op);
    if (added_muxes != nullptr && mux_added) {
      added_muxes->push_back(uni_op);
    }
  }

  return unified_operands;
}

absl::Status IntegrationFunction::DeUnifyIntegrationNodes(Node* node) {
  XLS_RET_CHECK_EQ(node->op(), Op::kSel);
  Select* mux = node->As<Select>();
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(mux));
  XLS_RET_CHECK_EQ(mux->cases().size(), 2);
  Node* a_in = mux->cases().at(0);
  Node* b_in = mux->cases().at(1);

  // Clean up bookkeeping.
  std::pair<const Node*, const Node*> key = std::make_pair(a_in, b_in);
  XLS_RET_CHECK(node_pair_to_mux_.contains(key));
  XLS_RET_CHECK_EQ(node_pair_to_mux_.at(key), mux);
  node_pair_to_mux_.erase(key);

  // Clean up nodes.
  Node* selector = mux->selector();
  XLS_RETURN_IF_ERROR(function()->RemoveNode(mux));
  if (selector->users().empty()) {
    XLS_RETURN_IF_ERROR(
        function()->RemoveNode(selector, /*remove_param_ok=*/true));
  }

  return absl::OkStatus();
}

bool IntegrationFunction::HasMapping(const Node* node) const {
  return original_node_to_integrated_node_map_.contains(node);
}

bool IntegrationFunction::IsMappingTarget(const Node* node) const {
  return integrated_node_to_original_nodes_map_.contains(node);
}

absl::StatusOr<Node*> IntegrationFunction::InsertNode(const Node* to_insert) {
  // TODO(jbaileyhandle): Implement this by calling MergeNodes behind
  // the scenes.
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(to_insert));
  XLS_RET_CHECK(!HasMapping(to_insert));
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> mapped_operands,
                       GetIntegratedOperands(to_insert));
  XLS_ASSIGN_OR_RETURN(Node * inserted, to_insert->CloneInNewFunction(
                                            mapped_operands, function()));
  XLS_RETURN_IF_ERROR(SetNodeMapping(to_insert, inserted));
  return inserted;
}

absl::StatusOr<absl::optional<absl::variant<Node*, float>>> 
  IntegrationFunction::MergeNodesBackend(const Node* node_a, const Node* node_b, bool score_only) {
  // Simple way to avoid situation where node_a depends on node_b or
  // vice versa. If we later want to merge nodes from the same function,
  // we can implement reaching analysis to check for dependencies.
  if(node_a != node_b) {
    XLS_RET_CHECK(node_a->function() != node_b->function());
  }
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(node_a) || !IntegrationFunctionOwnsNode(node_b));

  // Separate targets for node_a and node_b because we may derive
  // map target nodes from the merged node (e.g. if a and b have
  // different bitdwdiths, the merged node may take the maximum
  // width. We then add a bitslice to map the smaller node to).
  Node* a_target = nullptr;
  Node* b_target = nullptr;
  std::vector<Node*> added_muxes;
  std::vector<Node*> other_added_nodes;

  // Identical nodes can always be merged.
  if(node_a->IsDefinitelyEqualTo(node_b)) {
    XLS_ASSIGN_OR_RETURN(std::vector<Node*> operands, UnifyNodeOperands(node_a, node_b, &added_muxes));
    Node* merged = node_a->CloneInNewFunction(operands, function());
    a_target = merged;
    b_target = merged;
    other_added_nodes.push_back(merged);
    return true;
  } else {
    // TODO(jbaileyhandle): Add logic for nodes that are not identical but
    // may still be merged e.g. different bitwidths for bitwise ops.
    switch(node_a->op()) {
      default:
        return absl::nullopt;
        break;
    }
  }

  if(score_only) {
    // Score.
    float score = 0.0;
    if(

    // Cleanup.
  } else {
  }

  return absl::nullopt;
}

absl::StatusOr<Function*> IntegrationBuilder::CloneFunctionRecursive(
    const Function* function,
    absl::flat_hash_map<const Function*, Function*>* call_remapping) {
  // Collect callee functions.
  std::vector<const Function*> callee_funcs;
  for (const Node* node : function->nodes()) {
    switch (node->op()) {
      case Op::kCountedFor:
        callee_funcs.push_back(node->As<CountedFor>()->body());
        break;
      case Op::kMap:
        callee_funcs.push_back(node->As<Map>()->to_apply());
        break;
      case Op::kInvoke:
        callee_funcs.push_back(node->As<Invoke>()->to_apply());
        break;
      default:
        break;
    }
  }

  // Clone and call_remapping callees.
  for (const Function* callee : callee_funcs) {
    if (!call_remapping->contains(callee)) {
      XLS_ASSIGN_OR_RETURN(Function * callee_clone,
                           CloneFunctionRecursive(callee, call_remapping));
      (*call_remapping)[callee] = callee_clone;
    }
  }

  std::string clone_name =
      function_name_uniquer_.GetSanitizedUniqueName(function->name());
  return function->Clone(clone_name, package_.get(), *call_remapping);
}

absl::Status IntegrationBuilder::CopySourcesToIntegrationPackage() {
  source_functions_.reserve(original_package_source_functions_.size());
  for (const Function* source : original_package_source_functions_) {
    absl::flat_hash_map<const Function*, Function*> call_remapping;
    XLS_ASSIGN_OR_RETURN(Function * clone_func,
                         CloneFunctionRecursive(source, &call_remapping));
    source_functions_.push_back(clone_func);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<IntegrationBuilder>> IntegrationBuilder::Build(
    absl::Span<const Function* const> input_functions) {
  auto builder = absl::WrapUnique(new IntegrationBuilder(input_functions));

  // Add sources to common package.
  XLS_RETURN_IF_ERROR(builder->CopySourcesToIntegrationPackage());

  switch (builder->source_functions_.size()) {
    case 0:
      return absl::InternalError(
          "No source functions provided for integration");
    case 1:
      builder->integrated_function_ = builder->source_functions_.front();
      break;
    default:
      return absl::InternalError("Integration not yet implemented.");
  }

  return std::move(builder);
}

}  // namespace xls
