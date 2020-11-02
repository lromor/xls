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

#ifndef XLS_INTEGRATOR_IR_INTEGRATOR_H_
#define XLS_INTEGRATOR_IR_INTEGRATOR_H_

#include "absl/status/statusor.h"
#include "xls/ir/function.h"
#include "xls/ir/package.h"

namespace xls {

// Class used to integrate separate functions into a combined, reprogrammable
// circuit that can be configured to have the same functionality as the
// input functions. The builder will attempt to construct the integrated
// function such that hardware common to the input functions is consolidated.
// Note that this is distinct from function inlining. With inlining, a function
// call is replaced by the body of the function that is called.  With function
// integration, we take separate functions that do not call each other and
// combine the hardware used to implement the functions.
class IntegrationBuilder {
 public:
  IntegrationBuilder(absl::Span<const Function* const> input_functions) {
    // TODO(jbaileyhandle): Make package name an optional argument.
    original_package_source_functions_.insert(
        original_package_source_functions_.end(), input_functions.begin(),
        input_functions.end());
  }

  Package* package() { return package_.get(); }

  // Produce an integrated function implementing all
  // all functions in source_functions_.
  absl::StatusOr<Function*> Build();

 private:
  // Copy the source functions into a common package.
  absl::Status CopySourcesToIntegrationPackage();

  // Common package for to-be integrated functions
  // and integrated function.
  std::unique_ptr<Package> package_;

  // Functions to be integrated, in the integration package.
  std::vector<Function*> source_functions_;
  // Functions to be integrated, in their original packages.
  std::vector<const Function*> original_package_source_functions_;
};

}  // namespace xls

#endif  // XLS_INTEGRATOR_IR_INTEGRATOR_H_
