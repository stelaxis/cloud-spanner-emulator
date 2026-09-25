//
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
//

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_FUNCTION_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_FUNCTION_CATALOG_H_

#include <functional>
#include <memory>
#include <string>

#include "google/protobuf/struct.pb.h"
#include "googlesql/public/function.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "backend/common/case.h"
#include "backend/schema/catalog/schema.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

constexpr char kCloudSpannerEmulatorFunctionCatalogName[] = "Spanner";

std::unique_ptr<googlesql::Function> CreateSecureContextFunction(
    absl::string_view catalog_name,
    const absl::flat_hash_map<std::string, google::protobuf::Value>&
        secure_context);

// A catalog of all SQL functions.
//
// The FunctionCatalog supports looking up a function by name and enumerating
// all existing functions.
class FunctionCatalog {
 public:
  // catalog_name allows tests to override the catalog name.
  // Overriding the catalog name is required for some PG dialect testing.
  explicit FunctionCatalog(googlesql::TypeFactory* type_factory,
                           const std::string& catalog_name =
                               kCloudSpannerEmulatorFunctionCatalogName,
                           const backend::Schema* schema = nullptr);
  void GetFunction(const std::string& name,
                   const googlesql::Function** output) const;
  void GetFunctions(
      absl::flat_hash_set<const googlesql::Function*>* output) const;
  void GetTableValuedFunction(
      const std::string& name,
      const googlesql::TableValuedFunction** output) const;

  // Sets the latest schema that sequence and time zone functions use. Schema
  // changes replace it while queries and default-value expressions of
  // concurrent transactions evaluate those functions, so evaluators hold the
  // schema they loaded (GetLatestSchema) until they are done with it.
  void SetLatestSchema(std::shared_ptr<const backend::Schema> schema)
      ABSL_LOCKS_EXCLUDED(latest_schema_mu_);

  // As above, without ownership: the caller keeps `schema` alive for as long
  // as this catalog is used.
  void SetLatestSchema(const backend::Schema* schema)
      ABSL_LOCKS_EXCLUDED(latest_schema_mu_);

  std::shared_ptr<const backend::Schema> GetLatestSchema() const
      ABSL_LOCKS_EXCLUDED(latest_schema_mu_);

  // Runs `hook` each time a sequence or identity function has loaded the
  // latest schema to evaluate against. For tests.
  void set_schema_loaded_hook_for_testing(std::function<void()> hook) {
    schema_loaded_hook_ = std::move(hook);
  }

 private:
  void AddGoogleSQLBuiltInFunctions(googlesql::TypeFactory* type_factory);
  void AddPGLambdaFunctions();
  void AddSpannerFunctions();
  void AddGraphSafeToJsonSignatures();
  void AddMlFunctions(googlesql::TypeFactory* type_factory);
  void AddSearchFunctions(googlesql::TypeFactory* type_factory);

  void AddSpannerPGFunctions();
  void AddFunctionAliases();

  std::unique_ptr<googlesql::Function> GetPGToCharFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetPGExtractFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetPGCastToTimestampFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetPGCastToStringFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetPGDateTruncFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetInternalSequenceStateFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetTableColumnIdentityStateFunction(
      const std::string& catalog_name);

  std::unique_ptr<googlesql::Function> GetNextSequenceValueFunction(
      const std::string& catalog_name);

  CaseInsensitiveStringMap<std::unique_ptr<googlesql::Function>> functions_;
  CaseInsensitiveStringMap<std::unique_ptr<googlesql::TableValuedFunction>>
      table_valued_functions_;
  const std::string catalog_name_;
  // Returns the latest schema for a function evaluation, after running the
  // test hook.
  std::shared_ptr<const backend::Schema> LoadLatestSchemaForEvaluation() const;

  // The latest schema, since some functions need to access it (e.g. sequence
  // functions).
  mutable absl::Mutex latest_schema_mu_;
  std::shared_ptr<const backend::Schema> latest_schema_
      ABSL_GUARDED_BY(latest_schema_mu_);

  // See set_schema_loaded_hook_for_testing. Set before concurrent use.
  std::function<void()> schema_loaded_hook_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_FUNCTION_CATALOG_H_
