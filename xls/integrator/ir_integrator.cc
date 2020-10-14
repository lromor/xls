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

#include "xls/integrator/ir_integrator.h"

#include "xls/ir/ir_parser.h"

namespace xls {

// Return the name of 'function', prepended with the 'package' name.
std::string GetParsableQualifiedFunctionName(Package* package,
                                             const Function* function) {
  return "PKGzzz" + package->name() + "zzzFNzzz" + function->name();
}

absl::Status IntegrationBuilder::CopySourcesToIntegrationPackage() {
  std::string integration_package_str = "package IntegrationPackage\n";

  // Dump IR for original functions with qualified names.
  for (const auto* source : original_package_source_functions_) {
    // Copy functions to temporary package to avoid modifying
    // original functions.
    std::string srctmp_package_str = "package srctmp\n";
    srctmp_package_str.append(source->DumpIr(/*recursive=*/true));
    XLS_ASSIGN_OR_RETURN(auto srctmp_package,
                         Parser::ParsePackage(srctmp_package_str));

    // Change all function names before dumping any function so that
    // all function invocations refer to the new function name.
    for (const auto& function : srctmp_package->functions()) {
      function->SetName(
          GetParsableQualifiedFunctionName(source->package(), function.get()));
    }

    // Dump functions to common IR string.
    for (const auto& function : srctmp_package->functions()) {
      integration_package_str.append("\n");
      integration_package_str.append(function->DumpIr(/*recursive=*/false));
    }
  }

  // Parse funtions into common package.
  XLS_ASSIGN_OR_RETURN(package_, Parser::ParsePackage(integration_package_str));
  source_functions_.reserve(original_package_source_functions_.size());
  for (const auto* source : original_package_source_functions_) {
    XLS_ASSIGN_OR_RETURN(Function * new_func,
                         package_->GetFunction(GetParsableQualifiedFunctionName(
                             source->package(), source)));
    source_functions_.push_back(new_func);
  }

  return absl::OkStatus();
}

absl::StatusOr<Function*> IntegrationBuilder::Build() {
  // Add sources to common package.
  XLS_RETURN_IF_ERROR(CopySourcesToIntegrationPackage());

  switch (source_functions_.size()) {
    case 0:
      return absl::InternalError(
          "No source functions provided for integration");
    case 1:
      return source_functions_.front();
    default:
      return absl::InternalError("Integration not yet implemented.");
      break;
  }
}

}  // namespace xls
