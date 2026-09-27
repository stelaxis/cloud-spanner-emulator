//
// Copyright 2026 Google LLC
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

#include "backend/query/function_catalog.h"

#include <atomic>
#include <map>
#include <ostream>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/function.h"
#include "googlesql/public/table_valued_function.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "backend/schema/catalog/schema.h"
#include "common/constants.h"
#include "common/feature_flags.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

namespace database_api = ::google::spanner::admin::database::v1;

using ::testing::Values;

// Everything a query or schema change can observe of a catalog's functions,
// by name, except the identity of their evaluators.
std::map<std::string, std::string> Describe(const FunctionCatalog& catalog) {
  absl::flat_hash_set<const googlesql::Function*> functions;
  catalog.GetFunctions(&functions);
  std::map<std::string, std::string> described;
  for (const googlesql::Function* function : functions) {
    const googlesql::Function* by_name = nullptr;
    catalog.GetFunction(function->Name(), &by_name);
    const googlesql::FunctionOptions& options = function->function_options();
    std::string description = absl::StrCat(
        function->DebugString(/*verbose=*/true), "|group=",
        function->GetGroup(), "|mode=", function->mode(),
        "|alias=", function->alias_name(),
        "|evaluator=", function->GetFunctionEvaluatorFactory() != nullptr,
        "|aggregate_evaluator=",
        function->GetAggregateFunctionEvaluatorFactory() != nullptr,
        "|safe=", options.supports_safe_error_mode,
        "|coercible=", options.arguments_are_coercible,
        "|found_by_name=", by_name != nullptr);
    // Aliases are separate functions with the original's name, so key by the
    // description as well.
    described[absl::StrCat(function->Name(), "#", description)] = description;
  }
  return described;
}

std::map<std::string, std::string> DescribeTvfs(
    const FunctionCatalog& catalog, const std::vector<std::string>& names) {
  std::map<std::string, std::string> described;
  for (const std::string& name : names) {
    const googlesql::TableValuedFunction* tvf = nullptr;
    catalog.GetTableValuedFunction(name, &tvf);
    described[name] = tvf == nullptr ? "<none>" : tvf->DebugString();
  }
  return described;
}

const std::vector<std::string>& TvfNames() {
  static const auto* names =
      new std::vector<std::string>{"ML.PREDICT", "SAFE.ML.PREDICT",
                                   "no_such_tvf"};
  return *names;
}

std::unique_ptr<const Schema> SchemaWithOneSequence(
    googlesql::TypeFactory* type_factory,
    database_api::DatabaseDialect dialect) {
  absl::StatusOr<std::unique_ptr<const Schema>> schema =
      test::CreateSchemaWithOneSequence(type_factory, dialect);
  EXPECT_THAT(schema, googlesql_base::testing::IsOk());
  return *std::move(schema);
}

std::string SequenceArgument(database_api::DatabaseDialect dialect) {
  // GoogleSQL passes the algebrized name, PostgreSQL the literal.
  return dialect == database_api::DatabaseDialect::POSTGRESQL ? "myseq"
                                                              : "_sequence_myseq";
}

absl::StatusOr<googlesql::Value> NextSequenceValue(
    const FunctionCatalog& catalog, const std::string& argument) {
  const googlesql::Function* function = nullptr;
  catalog.GetFunction(kGetNextSequenceValueFunctionName, &function);
  if (function == nullptr) {
    return absl::NotFoundError("no get_next_sequence_value");
  }
  // The second signature takes the sequence name as a string.
  GOOGLESQL_ASSIGN_OR_RETURN(googlesql::FunctionEvaluator evaluator,
                   function->GetFunctionEvaluatorFactory()(
                       function->signatures().at(1)));
  return evaluator({googlesql::Value::String(argument)});
}

struct CatalogCase {
  std::string catalog_name;
  database_api::DatabaseDialect dialect;
};

void PrintTo(const CatalogCase& c, std::ostream* os) {
  *os << c.catalog_name << "/" << database_api::DatabaseDialect_Name(c.dialect);
}

class FunctionCatalogSharingTest
    : public ::testing::TestWithParam<CatalogCase> {
 protected:
  void TearDown() override {
    FunctionCatalog::SetSharingEnabledForTesting(true);
  }
  googlesql::TypeFactory type_factory_;
};

// A catalog built from shared functions has exactly the functions of one that
// builds them all itself.
TEST_P(FunctionCatalogSharingTest, SharedMatchesBuiltFromScratch) {
  std::unique_ptr<const Schema> schema =
      SchemaWithOneSequence(&type_factory_, GetParam().dialect);

  FunctionCatalog shared(&type_factory_, GetParam().catalog_name,
                         schema.get());
  ASSERT_TRUE(shared.uses_shared_functions());

  FunctionCatalog::SetSharingEnabledForTesting(false);
  FunctionCatalog unshared(&type_factory_, GetParam().catalog_name,
                           schema.get());
  ASSERT_FALSE(unshared.uses_shared_functions());

  std::map<std::string, std::string> shared_functions = Describe(shared);
  EXPECT_GT(shared_functions.size(), 400);
  EXPECT_EQ(shared_functions, Describe(unshared));
  std::map<std::string, std::string> shared_tvfs =
      DescribeTvfs(shared, TvfNames());
  EXPECT_NE(shared_tvfs["ML.PREDICT"], "<none>");
  EXPECT_EQ(shared_tvfs, DescribeTvfs(unshared, TvfNames()));
}

// Two catalogs share the functions that do not read the schema, and each has
// its own schema-bound functions, which read its own schema.
TEST_P(FunctionCatalogSharingTest, SchemaBoundFunctionsReadTheirOwnSchema) {
  const database_api::DatabaseDialect dialect = GetParam().dialect;
  std::unique_ptr<const Schema> with_sequence =
      SchemaWithOneSequence(&type_factory_, dialect);
  FunctionCatalog first(&type_factory_, GetParam().catalog_name,
                        with_sequence.get());
  FunctionCatalog second(&type_factory_, GetParam().catalog_name,
                         with_sequence.get());
  ASSERT_TRUE(first.uses_shared_functions());
  ASSERT_TRUE(second.uses_shared_functions());

  const googlesql::Function* first_concat = nullptr;
  const googlesql::Function* second_concat = nullptr;
  first.GetFunction("concat", &first_concat);
  second.GetFunction("concat", &second_concat);
  ASSERT_NE(first_concat, nullptr);
  EXPECT_EQ(first_concat, second_concat);

  for (const char* name :
       {kGetNextSequenceValueFunctionName,
        kGetInternalSequenceStateFunctionName,
        kGetTableColumnIdentityStateFunctionName, "pg.to_char", "pg.extract",
        "pg.cast_to_timestamp", "pg.cast_to_string", "pg.date_trunc"}) {
    const googlesql::Function* in_first = nullptr;
    const googlesql::Function* in_second = nullptr;
    first.GetFunction(name, &in_first);
    second.GetFunction(name, &in_second);
    ASSERT_NE(in_first, nullptr) << name;
    EXPECT_NE(in_first, in_second) << name;
  }

  // `second` now evaluates against a schema without the sequence; `first`
  // still has it.
  std::unique_ptr<const Schema> without_sequence =
      test::CreateSchemaFromDDL(
          {dialect == database_api::DatabaseDialect::POSTGRESQL
               ? "CREATE TABLE t (k bigint PRIMARY KEY)"
               : "CREATE TABLE t (k INT64) PRIMARY KEY (k)"},
          &type_factory_, "", dialect)
          .value();
  second.SetLatestSchema(without_sequence.get());
  EXPECT_THAT(NextSequenceValue(first, SequenceArgument(dialect)),
              googlesql_base::testing::IsOk());
  EXPECT_THAT(NextSequenceValue(second, SequenceArgument(dialect)),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

INSTANTIATE_TEST_SUITE_P(
    Catalogs, FunctionCatalogSharingTest,
    Values(CatalogCase{kCloudSpannerEmulatorFunctionCatalogName,
                       database_api::DatabaseDialect::GOOGLE_STANDARD_SQL},
           CatalogCase{kCloudSpannerEmulatorFunctionCatalogName,
                       database_api::DatabaseDialect::POSTGRESQL},
           CatalogCase{"pg", database_api::DatabaseDialect::POSTGRESQL}),
    [](const ::testing::TestParamInfo<CatalogCase>& info) {
      return absl::StrCat(info.param.catalog_name == "pg" ? "Pg" : "Spanner",
                          "_",
                          database_api::DatabaseDialect_Name(
                              info.param.dialect));
    });

// The functions of the GoogleSQL dialect differ from those of the PostgreSQL
// one, so they are not shared across dialects.
TEST(FunctionCatalogTest, DialectsDoNotShare) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> gsql = SchemaWithOneSequence(
      &type_factory, database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
  std::unique_ptr<const Schema> pg = SchemaWithOneSequence(
      &type_factory, database_api::DatabaseDialect::POSTGRESQL);
  FunctionCatalog gsql_catalog(&type_factory,
                               kCloudSpannerEmulatorFunctionCatalogName,
                               gsql.get());
  FunctionCatalog pg_catalog(&type_factory,
                             kCloudSpannerEmulatorFunctionCatalogName,
                             pg.get());
  EXPECT_NE(Describe(gsql_catalog), Describe(pg_catalog));
}

// Feature flags are part of the key: a catalog built under other flags gets
// other shared functions, equal to those it would build itself.
TEST(FunctionCatalogTest, FeatureFlagsKeyTheSharedFunctions) {
  googlesql::TypeFactory type_factory;
  FunctionCatalog with_defaults(&type_factory);
  const googlesql::Function* default_concat = nullptr;
  with_defaults.GetFunction("concat", &default_concat);

  EmulatorFeatureFlags::Flags flags;
  flags.enable_protos = false;
  test::ScopedEmulatorFeatureFlagsSetter setter(flags);
  FunctionCatalog shared(&type_factory);
  ASSERT_TRUE(shared.uses_shared_functions());
  const googlesql::Function* concat = nullptr;
  shared.GetFunction("concat", &concat);
  EXPECT_NE(concat, default_concat);

  FunctionCatalog::SetSharingEnabledForTesting(false);
  FunctionCatalog unshared(&type_factory);
  FunctionCatalog::SetSharingEnabledForTesting(true);
  EXPECT_EQ(Describe(shared), Describe(unshared));
}

// Catalogs are built and evaluated concurrently, for ThreadSanitizer, while
// another thread changes the feature flags.
TEST(FunctionCatalogTest, ConcurrentConstructionAndEvaluation) {
  googlesql::TypeFactory type_factory;
  std::unique_ptr<const Schema> gsql = SchemaWithOneSequence(
      &type_factory, database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
  std::unique_ptr<const Schema> pg = SchemaWithOneSequence(
      &type_factory, database_api::DatabaseDialect::POSTGRESQL);
  const EmulatorFeatureFlags::Flags original =
      EmulatorFeatureFlags::instance().flags();

  std::atomic<bool> done = false;
  std::thread flipper([&] {
    EmulatorFeatureFlags::Flags flags = original;
    auto& instance = const_cast<EmulatorFeatureFlags&>(
        EmulatorFeatureFlags::instance());
    while (!done) {
      flags.enable_protos = !flags.enable_protos;
      instance.set_flags(flags);
      std::this_thread::yield();
    }
    instance.set_flags(original);
  });
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 20; ++i) {
        const Schema* schema = (t + i) % 2 == 0 ? gsql.get() : pg.get();
        FunctionCatalog catalog(&type_factory,
                                kCloudSpannerEmulatorFunctionCatalogName,
                                schema);
        const googlesql::Function* concat = nullptr;
        catalog.GetFunction("concat", &concat);
        EXPECT_NE(concat, nullptr);
        EXPECT_THAT(NextSequenceValue(catalog,
                                      SequenceArgument(schema->dialect())),
                    googlesql_base::testing::IsOk());
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  done = true;
  flipper.join();
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
