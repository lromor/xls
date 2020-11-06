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
    const IntegrationOptions& options, std::string function_name) {
  // Create integration function object.
  auto integration_function =
      absl::WrapUnique(new IntegrationFunction(options));
  integration_function->package_ = package;

  // Create ir function.
  integration_function->function_ =
      absl::make_unique<Function>(function_name, package);

  // Package source function parameters as tuple parameters to integration
  // function.
  int64 source_function_idx = 0;
  for (const auto* source_func : source_functions) {
    // Record function index.
    integration_function->source_function_base_to_index_[source_func] =
        source_function_idx++;

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

  if (!integration_function->integration_options_
           .unique_select_signal_per_mux()) {
    int64 num_bits = Bits::MinBitCountUnsigned(source_functions.size() - 1);
    XLS_ASSIGN_OR_RETURN(
        integration_function->global_mux_select_param_,
        // TODO(jbaile): How many bits?
        integration_function->function_->MakeNodeWithName<Param>(
            /*loc=*/std::nullopt, std::string("global_mux_select"),
            integration_function->package_->GetBitsType(num_bits)));
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

absl::StatusOr<const std::set<int64>*>
IntegrationFunction::GetGlobalMuxOccupiedCaseIndexes(const Node* node) const {
  XLS_RET_CHECK(global_mux_to_metadata_.contains(node));
  return &global_mux_to_metadata_.at(node).occupied_case_indexes;
}

absl::StatusOr<const std::set<int64>*>
IntegrationFunction::GetGlobalMuxLastCaseIndexesAdded(const Node* node) const {
  XLS_RET_CHECK(global_mux_to_metadata_.contains(node));
  return &global_mux_to_metadata_.at(node).last_case_indexes_added;
}

int64 IntegrationFunction::GetNumberOfGlobalMuxesTracked() const {
  return global_mux_to_metadata_.size();
}

absl::StatusOr<int64> IntegrationFunction::GetSourceFunctionIndexOfNode(
    const Node* node) const {
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(node));
  XLS_RET_CHECK(source_function_base_to_index_.contains(node->function_base()));
  return source_function_base_to_index_.at(node->function_base());
}

absl::StatusOr<std::set<int64>>
IntegrationFunction::GetSourceFunctionIndexesOfNodesMappedToNode(
    const Node* map_target) const {
  XLS_ASSIGN_OR_RETURN(const absl::flat_hash_set<const Node*>* src_nodes,
                       GetNodesMappedToNode(map_target));

  std::set<int64> source_indexes;
  for (const auto* node : *src_nodes) {
    XLS_ASSIGN_OR_RETURN(int64 index, GetSourceFunctionIndexOfNode(node));
    source_indexes.insert(index);
  }
  return source_indexes;
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
    }
    XLS_ASSIGN_OR_RETURN(Node * operand_map_target, GetNodeMapping(operand));
    operand_mappings.push_back(operand_map_target);
  }
  return operand_mappings;
}

absl::StatusOr<IntegrationFunction::UnifiedNode>
IntegrationFunction::UnifyIntegrationNodes(Node* node_a, Node* node_b) {
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(node_a));
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(node_b));
  XLS_RET_CHECK_EQ(node_a->GetType(), node_b->GetType());

  // Same node.
  if (node_a == node_b) {
    return UnifiedNode({node_a, /*change=*/UnificationChange::kNoChange});
  }

  if (integration_options_.unique_select_signal_per_mux()) {
    return UnifyIntegrationNodesWithPerMuxSelect(node_a, node_b);
  } else {
    return UnifyIntegrationNodesWithGlobalMuxSelect(node_a, node_b);
  }
}

absl::StatusOr<IntegrationFunction::UnifiedNode>
IntegrationFunction::UnifyIntegrationNodesWithPerMuxSelect(Node* node_a,
                                                           Node* node_b) {
  // Already created a mux to select between these nodes.
  // Note that we search for (node_a, node_b) but not for
  // (node_b, node_a) because a reversed pair implies
  // an inverted select signal.
  // TODO(jbaileyhandle): Create a canonical ordering among nodes to avoid
  // this issue.
  std::pair<const Node*, const Node*> key = std::make_pair(node_a, node_b);
  if (node_pair_to_mux_.contains(key)) {
    return UnifiedNode(
        {node_pair_to_mux_.at(key), /*change=*/UnificationChange::kNoChange});
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
  return UnifiedNode({mux, /*change=*/UnificationChange::kNewMuxAdded});
}

absl::StatusOr<Node*> IntegrationFunction::ReplaceMuxCases(
    Node* mux_node,
    const absl::flat_hash_map<int64, Node*>& source_index_to_case) {
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(mux_node));
  XLS_RET_CHECK(!IsMappingTarget(mux_node));
  Select* mux = mux_node->As<Select>();

  // Add case.
  std::vector<Node*> new_cases;
  new_cases.insert(new_cases.end(), mux->cases().begin(), mux->cases().end());
  for (const auto& index_case : source_index_to_case) {
    XLS_RET_CHECK(IntegrationFunctionOwnsNode(index_case.second));
    new_cases[index_case.first] = index_case.second;
  }

  // Replace mux.
  XLS_ASSIGN_OR_RETURN(Node * new_mux,
                       function_->MakeNode<Select>(
                           /*loc=*/std::nullopt, mux->selector(), new_cases,
                           /*default_value=*/
                           IsPowerOfTwo(new_cases.size())
                               ? std::nullopt
                               : absl::optional<Node*>(mux->default_value())));
  XLS_RETURN_IF_ERROR(mux->ReplaceUsesWith(new_mux).status());
  XLS_RETURN_IF_ERROR(function()->RemoveNode(mux));

  return new_mux;
}

absl::StatusOr<IntegrationFunction::UnifiedNode>
IntegrationFunction::UnifyIntegrationNodesWithGlobalMuxSelect(Node* node_a,
                                                              Node* node_b) {
  // Determine if node_a or node_b is a mux previously generated by this
  // function.
  auto is_unify_generated_mux = [this](Node* node) {
    if (node->op() != Op::kSel) {
      return false;
    }
    Select* mux = node->As<Select>();
    Node* selector = mux->selector();
    return (selector == global_mux_select_param_);
  };
  bool a_is_unify_mux = is_unify_generated_mux(node_a);
  bool b_is_unify_mux = is_unify_generated_mux(node_b);
  XLS_RET_CHECK(!(a_is_unify_mux && b_is_unify_mux));

  // Neither input is a pre-existing mux.
  if (!a_is_unify_mux && !b_is_unify_mux) {
    return UnifyIntegrationNodesWithGlobalMuxSelectNoMuxArg(node_a, node_b);

    // One input is already a pre-existing mux.
  } else {
    // ID mux and new case for mux.
    Node* mux = a_is_unify_mux ? node_a : node_b;
    Node* case_node = !a_is_unify_mux ? node_a : node_b;

    return UnifyIntegrationNodesWithGlobalMuxSelectArgIsMux(mux, case_node);
  }
}

absl::StatusOr<IntegrationFunction::UnifiedNode>
IntegrationFunction::UnifyIntegrationNodesWithGlobalMuxSelectNoMuxArg(
    Node* node_a, Node* node_b) {
  // Prefer a canonical ordering of arguments to increase debuggability /
  // reduce arbitrariness.
  XLS_ASSIGN_OR_RETURN(std::set<int64> init_a_source_indexes,
                       GetSourceFunctionIndexesOfNodesMappedToNode(node_a));
  XLS_ASSIGN_OR_RETURN(std::set<int64> init_b_source_indexes,
                       GetSourceFunctionIndexesOfNodesMappedToNode(node_b));
  if (*init_a_source_indexes.begin() > *init_b_source_indexes.begin()) {
    std::swap(node_a, node_b);
  }

  // Create mux.
  std::vector<Node*> cases(source_function_base_to_index_.size(), node_a);
  XLS_ASSIGN_OR_RETURN(
      Node * init_mux,
      function_->MakeNode<Select>(
          /*loc=*/std::nullopt, global_mux_select_param_, cases,
          /*default_value=*/
          IsPowerOfTwo(cases.size()) ? std::nullopt
                                     : absl::optional<Node*>(node_a)));
  // Track assigned cases.
  XLS_ASSIGN_OR_RETURN(std::set<int64> a_source_indexes,
                       GetSourceFunctionIndexesOfNodesMappedToNode(node_a));
  GlobalMuxMetadata& init_metadata = global_mux_to_metadata_[init_mux];
  init_metadata.occupied_case_indexes = a_source_indexes;

  // Now handle as case where one input is a pre-existing mux.
  XLS_ASSIGN_OR_RETURN(
      UnifiedNode new_mux,
      UnifyIntegrationNodesWithGlobalMuxSelectArgIsMux(init_mux, node_b));
  new_mux.change = UnificationChange::kNewMuxAdded;

  // Track most recent assigned cases.
  GlobalMuxMetadata& new_mux_metadata =
      global_mux_to_metadata_.at(new_mux.node);
  new_mux_metadata.last_case_indexes_added.insert(a_source_indexes.begin(),
                                                  a_source_indexes.end());
  return new_mux;
}

absl::StatusOr<IntegrationFunction::UnifiedNode>
IntegrationFunction::UnifyIntegrationNodesWithGlobalMuxSelectArgIsMux(
    Node* mux, Node* case_node) {
  // Clear bookkeeping.
  XLS_RET_CHECK(global_mux_to_metadata_.contains(mux));
  std::set<int64> occupied =
      global_mux_to_metadata_.at(mux).occupied_case_indexes;
  global_mux_to_metadata_.erase(mux);

  // For each source function that maps a node to case_node,
  // insert case_node as the corresponding mux case.
  absl::flat_hash_map<int64, Node*> source_index_to_case;
  XLS_ASSIGN_OR_RETURN(std::set<int64> case_source_indexes,
                       GetSourceFunctionIndexesOfNodesMappedToNode(case_node));
  for (int64 index : case_source_indexes) {
    source_index_to_case[index] = case_node;
    XLS_RET_CHECK(occupied.find(index) == occupied.end());
  }
  XLS_ASSIGN_OR_RETURN(Node * new_mux,
                       ReplaceMuxCases(mux, source_index_to_case));

  // Update bookkeeping.
  GlobalMuxMetadata& new_metadata = global_mux_to_metadata_[new_mux];
  new_metadata.last_case_indexes_added = case_source_indexes;
  new_metadata.occupied_case_indexes = occupied;
  new_metadata.occupied_case_indexes.insert(case_source_indexes.begin(),
                                            case_source_indexes.end());

  return UnifiedNode(
      {new_mux, /*change=*/UnificationChange::kExistingMuxCasesModified});
}

absl::StatusOr<IntegrationFunction::UnifiedOperands>
IntegrationFunction::UnifyNodeOperands(const Node* node_a, const Node* node_b) {
  XLS_RET_CHECK_EQ(node_a->operands().size(), node_b->operands().size());

  // Get mapped operands.
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> a_ops, GetIntegratedOperands(node_a));
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> b_ops, GetIntegratedOperands(node_b));

  // Unify operands.
  UnifiedOperands unify_result;
  unify_result.operands.reserve(a_ops.size());
  for (int64 idx = 0; idx < a_ops.size(); ++idx) {
    XLS_ASSIGN_OR_RETURN(UnifiedNode uni_operand,
                         UnifyIntegrationNodes(a_ops.at(idx), b_ops.at(idx)));
    unify_result.operands.push_back(uni_operand.node);
    if (uni_operand.change != UnificationChange::kNoChange) {
      unify_result.changed_muxes.push_back(uni_operand);
    }
  }

  return unify_result;
}

absl::StatusOr<Node*> IntegrationFunction::DeUnifyIntegrationNodes(Node* node) {
  XLS_RET_CHECK_EQ(node->op(), Op::kSel);
  XLS_RET_CHECK(IntegrationFunctionOwnsNode(node));
  XLS_RET_CHECK(!IsMappingTarget(node));

  if (integration_options_.unique_select_signal_per_mux()) {
    XLS_RETURN_IF_ERROR(DeUnifyIntegrationNodesWithPerMuxSelect(node));
    return static_cast<Node*>(nullptr);
  } else {
    return DeUnifyIntegrationNodesWithGlobalMuxSelect(node);
  }
}

absl::Status IntegrationFunction::DeUnifyIntegrationNodesWithPerMuxSelect(
    Node* node) {
  Select* mux = node->As<Select>();
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
  XLS_RET_CHECK(mux->users().empty());
  XLS_RETURN_IF_ERROR(function()->RemoveNode(mux));
  XLS_RET_CHECK(selector->users().empty());
  XLS_RETURN_IF_ERROR(
      function()->RemoveNode(selector, /*remove_param_ok=*/true));

  return absl::OkStatus();
}

absl::StatusOr<Node*>
IntegrationFunction::DeUnifyIntegrationNodesWithGlobalMuxSelect(Node* node) {
  Select* mux = node->As<Select>();
  XLS_RET_CHECK_EQ(mux->cases().size(), source_function_base_to_index_.size());

  // Get bookkeeping.
  XLS_RET_CHECK(global_mux_to_metadata_.contains(node));
  GlobalMuxMetadata& metadata = global_mux_to_metadata_.at(node);
  XLS_RET_CHECK(!metadata.last_case_indexes_added.empty());

  // Clear occupied cases.
  for (int64 updated_idx : metadata.last_case_indexes_added) {
    metadata.occupied_case_indexes.erase(updated_idx);
  }

  // No more occupied cases - remove mux.
  if (metadata.occupied_case_indexes.empty()) {
    // Update bookkeeping.
    global_mux_to_metadata_.erase(node);

    XLS_RETURN_IF_ERROR(function()->RemoveNode(node));
    return static_cast<Node*>(nullptr);

    // Reset last updated cases.
  } else {
    Node* reset_value =
        mux->cases().at(*metadata.occupied_case_indexes.begin());
    absl::flat_hash_map<int64, Node*> case_updates;
    for (int64 updated_idx : metadata.last_case_indexes_added) {
      case_updates[updated_idx] = reset_value;
    }
    XLS_ASSIGN_OR_RETURN(Node * new_mux, ReplaceMuxCases(node, case_updates));

    // Update bookkeeping.
    GlobalMuxMetadata& new_metadata = global_mux_to_metadata_[new_mux];
    new_metadata = metadata;
    // Note: DeUnifyIntegrationNodes shouldn't be called repeatedly on
    // a mux, so we don't need to recover any updates past the update
    // that was just reversed. So simply clearing last_case_indexes_added
    // is okay.
    new_metadata.last_case_indexes_added.clear();
    global_mux_to_metadata_.erase(node);

    return new_mux;
  }
}

bool IntegrationFunction::HasMapping(const Node* node) const {
  return original_node_to_integrated_node_map_.contains(node);
}

bool IntegrationFunction::IsMappingTarget(const Node* node) const {
  return integrated_node_to_original_nodes_map_.contains(node);
}

absl::StatusOr<Node*> IntegrationFunction::InsertNode(Node* to_insert) {
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(to_insert));
  XLS_ASSIGN_OR_RETURN(std::vector<Node*> node_mappings,
                       MergeNodes(to_insert, to_insert));
  XLS_RET_CHECK_EQ(node_mappings.size(), 1);
  return node_mappings.front();
}

absl::StatusOr<float> IntegrationFunction::GetInsertNodeCost(
    const Node* to_insert) {
  XLS_RET_CHECK(!IntegrationFunctionOwnsNode(to_insert));
  XLS_ASSIGN_OR_RETURN(std::optional<float> insert_cost,
                       GetMergeNodesCost(to_insert, to_insert));
  XLS_RET_CHECK(insert_cost.has_value());
  return insert_cost.value();
}

absl::StatusOr<IntegrationFunction::MergeNodesBackendResult>
IntegrationFunction::MergeNodesBackend(const Node* node_a, const Node* node_b) {
  auto validate_node = [this](const Node* node) {
    if (IntegrationFunctionOwnsNode(node)) {
      if (!IsMappingTarget(node)) {
        return absl::InternalError(
            "Trying to merge non-mapping-target integration node.");
      }
    } else {
      if (HasMapping(node)) {
        return absl::InternalError(
            "Trying to merge non-integration node that already has mapping");
      }
    }
    return absl::OkStatus();
  };
  XLS_RETURN_IF_ERROR(validate_node(node_a));
  XLS_RETURN_IF_ERROR(validate_node(node_b));
  // Simple way to avoid situation where node_a depends on node_b or
  // vice versa. If we later want to merge nodes from the same function,
  // we can implement reaching analysis to check for dependencies.
  if (node_a != node_b) {
    XLS_RET_CHECK(node_a->function_base() != node_b->function_base());
  }

  // Separate targets for node_a and node_b because we may derive
  // map target nodes from the merged node;

  // Identical nodes can always be merged.
  if (node_a->IsDefinitelyEqualTo(node_b)) {
    XLS_ASSIGN_OR_RETURN(UnifiedOperands unified_operands,
                         UnifyNodeOperands(node_a, node_b));
    XLS_ASSIGN_OR_RETURN(
        Node * merged,
        node_a->CloneInNewFunction(unified_operands.operands, function()));

    return MergeNodesBackendResult{
        .can_merge = true,
        .target_a = merged,
        .target_b = merged,
        .changed_muxes = unified_operands.changed_muxes,
        .other_added_nodes = {merged}};
  } else {
    // TODO(jbaileyhandle): Add logic for nodes that are not identical but
    // may still be merged e.g. different bitwidths for bitwise ops.
    switch (node_a->op()) {
      default:
        break;
    }
  }

  return MergeNodesBackendResult{.can_merge = false};
}

absl::StatusOr<std::optional<int64>> IntegrationFunction::GetMergeNodesCost(
    const Node* node_a, const Node* node_b) {
  XLS_ASSIGN_OR_RETURN(MergeNodesBackendResult merge_result,
                       MergeNodesBackend(node_a, node_b));
  // Can't merge nodes.
  if (!merge_result.can_merge) {
    return absl::nullopt;
  }

  // Score.
  int64 cost = 0;
  auto tabulate_node_elimination_cost = [this, &cost](const Node* node) {
    if (IntegrationFunctionOwnsNode(node)) {
      cost -= GetNodeCost(node);
    }
  };
  tabulate_node_elimination_cost(node_a);
  tabulate_node_elimination_cost(node_b);
  for (UnifiedNode& unified_node : merge_result.changed_muxes) {
    if (unified_node.change == UnificationChange::kNewMuxAdded) {
      cost += GetNodeCost(unified_node.node);
    }
  }
  for (Node* new_node : merge_result.other_added_nodes) {
    cost += GetNodeCost(new_node);
  }

  // Cleanup.
  int64 nodes_eliminated = 0;
  int64 num_other_nodes_to_remove = merge_result.other_added_nodes.size();
  while (nodes_eliminated < num_other_nodes_to_remove) {
    int64 initial_eliminated = nodes_eliminated;
    for (auto list_itr = merge_result.other_added_nodes.begin();
         list_itr != merge_result.other_added_nodes.end();) {
      Node* new_node = *list_itr;
      if (new_node->users().empty()) {
        list_itr = merge_result.other_added_nodes.erase(list_itr);
        XLS_RETURN_IF_ERROR(function()->RemoveNode(new_node));
        ++nodes_eliminated;
      } else {
        ++list_itr;
      }
    }
    XLS_RET_CHECK_GT(nodes_eliminated, initial_eliminated);
  }

  for (UnifiedNode& unified_node : merge_result.changed_muxes) {
    XLS_RET_CHECK(unified_node.change != UnificationChange::kNoChange);
    XLS_RETURN_IF_ERROR(DeUnifyIntegrationNodes(unified_node.node).status());
  }

  return cost;
}

int64 IntegrationFunction::GetNodeCost(const Node* node) const {
  // TODO: Actual estimate.
  switch (node->op()) {
    case Op::kArray:
    case Op::kConcat:
    case Op::kBitSlice:
    case Op::kIdentity:
    case Op::kParam:
    case Op::kReverse:
    case Op::kSignExt:
    case Op::kTuple:
    case Op::kTupleIndex:
    case Op::kZeroExt:
      return 0;
      break;
    default:
      return 1;
      break;
  }
}

absl::StatusOr<std::vector<Node*>> IntegrationFunction::MergeNodes(
    Node* node_a, Node* node_b) {
  XLS_ASSIGN_OR_RETURN(MergeNodesBackendResult merge_result,
                       MergeNodesBackend(node_a, node_b));
  // Can't merge nodes.
  XLS_RET_CHECK(merge_result.can_merge);
  XLS_RET_CHECK(merge_result.target_a != nullptr);
  XLS_RET_CHECK(merge_result.target_b != nullptr);

  // Commit changes.
  auto commit_new_node = [this](Node* original, Node* target) -> absl::Status {
    XLS_RETURN_IF_ERROR(SetNodeMapping(original, target));
    if (IntegrationFunctionOwnsNode(original)) {
      XLS_RETURN_IF_ERROR(original->ReplaceUsesWith(target).status());
      XLS_RETURN_IF_ERROR(function()->RemoveNode(original));
    }
    return absl::OkStatus();
  };
  XLS_RETURN_IF_ERROR(commit_new_node(node_a, merge_result.target_a));
  XLS_RETURN_IF_ERROR(commit_new_node(node_b, merge_result.target_b));

  std::vector<Node*> result = {merge_result.target_a};
  if (merge_result.target_a != merge_result.target_b) {
    result.push_back(merge_result.target_b);
  }
  return result;
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
    absl::Span<const Function* const> input_functions,
    const IntegrationOptions& options) {
  auto builder =
      absl::WrapUnique(new IntegrationBuilder(input_functions, options));

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
