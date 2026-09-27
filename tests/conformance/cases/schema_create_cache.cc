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

// Differential tests of the CreateDatabase schema cache: databases created
// from the cache and from DDL must be indistinguishable through the API.

#include <algorithm>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "google/cloud/spanner/admin/instance_admin_client.h"
#include "google/cloud/spanner/client.h"
#include "google/cloud/spanner/create_instance_request_builder.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "backend/database/schema_create_cache.h"
#include "backend/schema/updater/schema_updater.h"
#include "common/feature_flags.h"
#include "tests/common/change_streams.h"
#include "tests/common/file_based_schema_reader.h"
#include "tests/common/file_based_test_runner.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/common/test.pb.h"
#include "tests/conformance/common/database_test_base.h"
#include "tests/conformance/common/environment.h"
#include "grpcpp/client_context.h"
#include "re2/re2.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using ::google::spanner::emulator::backend::SchemaChangeOperation;
using ::google::spanner::emulator::backend::SchemaCreateCache;

// The instance that holds the databases created without the cache. Each
// database there has the same ID as its cached counterpart in the test
// instance, since INFORMATION_SCHEMA shows it.
constexpr char kReferenceInstance[] = "cache-reference";

// A list of DDL statements to create a database with.
struct DdlCase {
  std::string name;
  DatabaseDialect dialect;
  std::vector<std::string> statements;
};

void PrintTo(const DdlCase& c, std::ostream* os) { *os << c.name; }

std::vector<std::string> SplitStatements(std::string text) {
  absl::StripAsciiWhitespace(&text);
  std::vector<std::string> statements;
  for (absl::string_view statement :
       absl::StrSplit(text, ';', absl::SkipWhitespace())) {
    statements.emplace_back(absl::StripAsciiWhitespace(statement));
  }
  return statements;
}

std::vector<std::string> TestFiles(const std::string& dir) {
  std::vector<std::string> files;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(GetRunfilesDir(dir), error)) {
    if (entry.path().extension() == ".test") {
      files.push_back(entry.path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// Every schema change test case and every conformance schema, as a create
// request.
std::vector<DdlCase> AllDdlCases() {
  std::vector<DdlCase> cases;
  for (const auto& [dir, dialect] :
       std::vector<std::pair<std::string, DatabaseDialect>>{
           {"tests/conformance/data/schema_changes",
            DatabaseDialect::GOOGLE_STANDARD_SQL},
           {"tests/conformance/data/schema_changes/pg",
            DatabaseDialect::POSTGRESQL}}) {
    for (const std::string& file : TestFiles(dir)) {
      for (const FileBasedTestCase& test_case :
           ReadTestCasesFromFile(file, FileBasedTestOptions{}, dialect)) {
        std::vector<std::string> statements =
            SplitStatements(test_case.input.text);
        if (statements.empty()) continue;
        cases.push_back(
            {absl::StrCat(std::filesystem::path(file).stem().string(), "_",
                          test_case.input.line_no),
             dialect, std::move(statements)});
      }
    }
  }
  for (const std::string& file :
       TestFiles("tests/conformance/data/schemas")) {
    absl::StatusOr<FileBasedSchemaSet> schema_set =
        ReadSchemaSetFromFile(file, FileBasedSchemaSetOptions{});
    if (!schema_set.ok()) continue;
    for (const auto& [dialect, text] : schema_set->schemas) {
      std::vector<std::string> statements = SplitStatements(text);
      if (statements.empty()) continue;
      cases.push_back({absl::StrCat("schema_",
                                    std::filesystem::path(file).stem().string(),
                                    "_", DatabaseDialect_Name(dialect)),
                       dialect, std::move(statements)});
    }
  }
  // A DDL file named by the environment, such as an application's schema.
  if (const char* path = std::getenv("SCHEMA_CREATE_CACHE_TEST_DDL")) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    cases.push_back({"extra_ddl", DatabaseDialect::GOOGLE_STANDARD_SQL,
                     SplitStatements(text.str())});
  }
  for (DdlCase& c : cases) {
    for (char& ch : c.name) {
      if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
    }
  }
  return cases;
}

// What a client can observe of a database, or of a request that failed.
struct Observed {
  absl::StatusCode code = absl::StatusCode::kOk;
  std::string message;
  std::vector<std::string> ddl;
  // "TABLE: metadata" then "TABLE: row" for every INFORMATION_SCHEMA table.
  std::vector<std::string> information_schema;
};

class SchemaCreateCacheTest : public DatabaseTest {
 public:
  SchemaCreateCacheTest()
      : flag_setter_({
            .enable_generated_pk = true,
            .enable_postgresql_interface = true,
            .enable_fk_delete_cascade_action = true,
            .enable_fk_enforcement_option = true,
            .enable_interleave_in = true,
        }) {}

  absl::Status SetUpDatabase() override { return absl::OkStatus(); }

  void SetUp() override {
    DatabaseTest::SetUp();
    previous_ = SchemaCreateCache::SetGlobalForTesting(&cache_);
    GOOGLESQL_ASSERT_OK(CreateReferenceInstance());
  }

  void TearDown() override {
    absl::MutexLock lock(created_mu_);
    for (const std::string& database : created_) {
      grpc::ClientContext context;
      google::protobuf::Empty empty;
      database_api::DropDatabaseRequest request;
      request.set_database(database);
      raw_database_client()->DropDatabase(&context, request, &empty);
    }
    SchemaCreateCache::SetGlobalForTesting(previous_);
    DatabaseTest::TearDown();
  }

 protected:
  // A valid database ID for the test's short name `id`.
  static std::string DbId(absl::string_view id) {
    return absl::StrCat("cache-", id);
  }

  std::string InstanceUri(bool reference) const {
    const ConformanceTestGlobals& globals = GetConformanceTestGlobals();
    return absl::StrCat("projects/", globals.project_id, "/instances/",
                        reference ? kReferenceInstance : globals.instance_id);
  }

  absl::Status CreateReferenceInstance() {
    const ConformanceTestGlobals& globals = GetConformanceTestGlobals();
    cloud::spanner_admin::InstanceAdminClient client(
        cloud::spanner_admin::MakeInstanceAdminConnection(
            *globals.connection_options));
    cloud::spanner::Instance instance(globals.project_id, kReferenceInstance);
    auto created =
        client
            .CreateInstance(cloud::spanner::CreateInstanceRequestBuilder(
                                instance, "test-config")
                                .SetDisplayName(kReferenceInstance)
                                .SetNodeCount(1)
                                .Build())
            .get();
    if (created.ok() ||
        created.status().code() == google::cloud::StatusCode::kAlreadyExists) {
      return absl::OkStatus();
    }
    return absl::InternalError(created.status().message());
  }

  std::unique_ptr<Client> MakeClient(bool reference, const std::string& id) {
    const ConformanceTestGlobals& globals = GetConformanceTestGlobals();
    cloud::spanner::Database database(
        cloud::spanner::Instance(
            globals.project_id,
            reference ? kReferenceInstance : globals.instance_id),
        DbId(id));
    return std::make_unique<Client>(
        cloud::spanner::MakeConnection(database, *globals.connection_options));
  }

  absl::StatusOr<std::string> CreateSession(const std::string& database) {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(database);
    spanner_api::Session session;
    GOOGLESQL_RETURN_IF_ERROR(
        ToAbslStatus(raw_client()->CreateSession(&context, request, &session)));
    return session.name();
  }

  static absl::Status ToAbslStatus(const grpc::Status& status) {
    return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                        status.error_message());
  }

  // Creates `id` in the reference instance without the cache, or in the test
  // instance with it.
  absl::Status Create(bool reference, const std::string& id,
                      const DdlCase& ddl) {
    SchemaCreateCache* cache =
        SchemaCreateCache::SetGlobalForTesting(reference ? nullptr : &cache_);
    absl::Status status = CreateUsingCurrentCache(reference, id, ddl);
    SchemaCreateCache::SetGlobalForTesting(cache);
    return status;
  }

  absl::Status CreateUsingCurrentCache(bool reference, const std::string& id,
                                       const DdlCase& ddl) {
    database_api::CreateDatabaseRequest request;
    request.set_parent(InstanceUri(reference));
    request.set_create_statement(
        ddl.dialect == DatabaseDialect::POSTGRESQL
            ? absl::StrCat("CREATE DATABASE \"", DbId(id), "\"")
            : absl::StrCat("CREATE DATABASE `", DbId(id), "`"));
    for (const std::string& statement : ddl.statements) {
      request.add_extra_statements(statement);
    }
    request.set_database_dialect(ddl.dialect);
    request.set_proto_descriptors(ProtoDescriptors(ddl.dialect));
    grpc::ClientContext context;
    operations_api::Operation operation;
    grpc::Status status =
        raw_database_client()->CreateDatabase(&context, request, &operation);
    if (!status.ok()) return ToAbslStatus(status);
    absl::MutexLock lock(created_mu_);
    created_.push_back(
        absl::StrCat(InstanceUri(reference), "/databases/", DbId(id)));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    if (operation.has_error()) {
      return absl::Status(
          static_cast<absl::StatusCode>(operation.error().code()),
          operation.error().message());
    }
    return absl::OkStatus();
  }

  static std::string ProtoDescriptors(DatabaseDialect dialect) {
    if (dialect == DatabaseDialect::POSTGRESQL) return "";
    google::protobuf::FileDescriptorSet files;
    ::emulator::tests::common::Simple::descriptor()->file()->CopyTo(
        files.add_file());
    return files.SerializeAsString();
  }

  absl::Status Update(bool reference, const std::string& id,
                      const std::vector<std::string>& statements,
                      DatabaseDialect dialect =
                          DatabaseDialect::GOOGLE_STANDARD_SQL) {
    database_api::UpdateDatabaseDdlRequest request;
    request.set_database(
        absl::StrCat(InstanceUri(reference), "/databases/", DbId(id)));
    for (const std::string& statement : statements) {
      request.add_statements(statement);
    }
    request.set_proto_descriptors(ProtoDescriptors(dialect));
    grpc::ClientContext context;
    operations_api::Operation operation;
    grpc::Status status = raw_database_client()->UpdateDatabaseDdl(
        &context, request, &operation);
    if (!status.ok()) return ToAbslStatus(status);
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    if (operation.has_error()) {
      return absl::Status(
          static_cast<absl::StatusCode>(operation.error().code()),
          operation.error().message());
    }
    return absl::OkStatus();
  }

  absl::StatusOr<spanner_api::ResultSet> Query(const std::string& database,
                                               const std::string& sql) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::string session, CreateSession(database));
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql(sql);
    request.mutable_transaction()->mutable_single_use()->mutable_read_only()
        ->set_strong(true);
    spanner_api::ResultSet result;
    grpc::ClientContext context;
    GOOGLESQL_RETURN_IF_ERROR(
        ToAbslStatus(raw_client()->ExecuteSql(&context, request, &result)));
    return result;
  }

  absl::StatusOr<Observed> Observe(bool reference, const std::string& id,
                                   DatabaseDialect dialect,
                                   const absl::Status& status) {
    Observed observed{.code = status.code(),
                      .message = std::string(status.message())};
    if (!status.ok()) return observed;
    const std::string database =
        absl::StrCat(InstanceUri(reference), "/databases/", DbId(id));
    {
      grpc::ClientContext context;
      database_api::GetDatabaseDdlRequest request;
      request.set_database(database);
      database_api::GetDatabaseDdlResponse response;
      GOOGLESQL_RETURN_IF_ERROR(ToAbslStatus(
          raw_database_client()->GetDatabaseDdl(&context, request, &response)));
      observed.ddl.assign(response.statements().begin(),
                          response.statements().end());
    }
    const bool pg = dialect == DatabaseDialect::POSTGRESQL;
    GOOGLESQL_ASSIGN_OR_RETURN(
        spanner_api::ResultSet tables,
        Query(database,
              pg ? "SELECT table_name FROM information_schema.tables WHERE "
                   "table_schema = 'information_schema' ORDER BY table_name"
                 : "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE "
                   "TABLE_SCHEMA = 'INFORMATION_SCHEMA' ORDER BY TABLE_NAME"));
    for (const auto& row : tables.rows()) {
      const std::string& table = row.values(0).string_value();
      GOOGLESQL_ASSIGN_OR_RETURN(
          spanner_api::ResultSet result,
          Query(database, absl::StrCat("SELECT * FROM ",
                                       pg ? "information_schema."
                                          : "INFORMATION_SCHEMA.",
                                       table)));
      observed.information_schema.push_back(
          absl::StrCat(table, ": ", result.metadata().row_type().DebugString()));
      // Without ORDER BY, rows come in hash-map order, which differs between
      // two databases created from the same DDL without the cache too.
      std::vector<std::string> rows;
      for (const auto& value : result.rows()) {
        rows.push_back(absl::StrCat(table, ": ", value.ShortDebugString()));
      }
      std::sort(rows.begin(), rows.end());
      observed.information_schema.insert(observed.information_schema.end(),
                                         rows.begin(), rows.end());
    }
    return observed;
  }

  // Fails with the first rows where `cached` and `reference` differ.
  static void ExpectSameRows(absl::string_view what,
                             const std::vector<std::string>& cached,
                             const std::vector<std::string>& reference) {
    if (cached == reference) return;
    std::string diff;
    int shown = 0;
    for (size_t i = 0; i < std::max(cached.size(), reference.size()); ++i) {
      const std::string c = i < cached.size() ? cached[i] : "<none>";
      const std::string r = i < reference.size() ? reference[i] : "<none>";
      if (c == r) continue;
      absl::StrAppend(&diff, "\n#", i, " cached:    ", c, "\n#", i,
                      " reference: ", r);
      if (++shown == 5) break;
    }
    ADD_FAILURE() << what << " differ (" << cached.size() << " vs "
                  << reference.size() << " rows):" << diff;
  }

  // Removes the orders that differ between two databases created from the
  // same DDL without the cache, and nothing else:
  // * PostgreSQL GetDatabaseDdl prints Schema::Dump(), whose order of
  //   statements varies; each statement is still compared byte for byte.
  // * A property graph label's property names are an absl::flat_hash_set,
  //   whose iteration order is salted per instance.
  static Observed Normalize(Observed observed, DatabaseDialect dialect) {
    if (dialect == DatabaseDialect::POSTGRESQL) {
      std::sort(observed.ddl.begin(), observed.ddl.end());
    }
    static const LazyRE2 kPropertyNames = {
        R"re(\\"propertyDeclarationNames\\":\[([^\]]*)\])re"};
    for (std::string& row : observed.information_schema) {
      std::string out;
      absl::string_view in(row), names;
      const char* done = row.data();
      while (RE2::FindAndConsume(&in, *kPropertyNames, &names)) {
        std::vector<std::string> sorted = absl::StrSplit(names, ',');
        std::sort(sorted.begin(), sorted.end());
        absl::StrAppend(&out, absl::string_view(done, names.data() - done),
                        absl::StrJoin(sorted, ","));
        done = names.data() + names.size();
      }
      if (!out.empty()) {
        absl::StrAppend(&out,
                        absl::string_view(done, row.data() + row.size() - done));
        row = std::move(out);
      }
    }
    return observed;
  }

  void ExpectSame(const Observed& cached_observed,
                  const Observed& reference_observed,
                  DatabaseDialect dialect) {
    const Observed cached = Normalize(cached_observed, dialect);
    const Observed reference = Normalize(reference_observed, dialect);
    EXPECT_EQ(cached.code, reference.code);
    EXPECT_EQ(cached.message, reference.message);
    ExpectSameRows("GetDatabaseDdl", cached.ddl, reference.ddl);
    ExpectSameRows("INFORMATION_SCHEMA", cached.information_schema,
                   reference.information_schema);
  }

  // Creates `id` without the cache, and with it after one create that fills
  // the cache, and returns what each observes.
  std::pair<Observed, Observed> CreateBoth(const std::string& id,
                                           const DdlCase& ddl) {
    absl::Status reference_status = Create(/*reference=*/true, id, ddl);
    const int size = cache_.size();
    absl::Status warm_status =
        Create(/*reference=*/false, absl::StrCat(id, "w"), ddl);
    const int64_t hits = cache_.hits();
    absl::Status cached_status = Create(/*reference=*/false, id, ddl);
    // Its database has another name, which an error message may include.
    EXPECT_EQ(warm_status.code(), reference_status.code());
    if (reference_status.ok()) {
      // The second create came from the cache, unless the schema is one the
      // cache cannot copy.
      std::shared_ptr<const SchemaCreateCache::Entry> entry =
          cache_.Peek(SchemaCreateCache::Key(Operation(ddl)));
      EXPECT_NE(entry, nullptr);
      EXPECT_EQ(cache_.hits(), hits + 1);
      if (entry != nullptr && entry->schema != nullptr) ++copied_;
    } else {
      // Failed creates are not cached.
      EXPECT_EQ(cache_.size(), size);
      EXPECT_EQ(cache_.hits(), hits);
    }
    absl::StatusOr<Observed> reference =
        Observe(/*reference=*/true, id, ddl.dialect, reference_status);
    absl::StatusOr<Observed> cached =
        Observe(/*reference=*/false, id, ddl.dialect, cached_status);
    EXPECT_THAT(reference, googlesql_base::testing::IsOk());
    EXPECT_THAT(cached, googlesql_base::testing::IsOk());
    return {cached.value_or(Observed{}), reference.value_or(Observed{})};
  }

  SchemaChangeOperation Operation(const DdlCase& ddl) {
    proto_descriptors_ = ProtoDescriptors(ddl.dialect);
    return SchemaChangeOperation{.statements = ddl.statements,
                                 .proto_descriptor_bytes = proto_descriptors_,
                                 .database_dialect = ddl.dialect};
  }

  const ScopedEmulatorFeatureFlagsSetter flag_setter_;
  SchemaCreateCache cache_{8};
  SchemaCreateCache* previous_ = nullptr;
  absl::Mutex created_mu_;
  // Databases to drop after the test. Tests create databases concurrently.
  std::vector<std::string> created_ ABSL_GUARDED_BY(created_mu_);
  std::string proto_descriptors_;
  int copied_ = 0;
};

class SchemaCreateCacheDdlTest
    : public SchemaCreateCacheTest,
      public ::testing::WithParamInterface<DdlCase> {};

// Every DDL list creates the same database, or fails with the same error,
// with the cache and without.
TEST_P(SchemaCreateCacheDdlTest, CreateMatchesUncached) {
  auto [cached, reference] = CreateBoth("d", GetParam());
  ExpectSame(cached, reference, GetParam().dialect);
}

// Applying the second half of each DDL list to a database created from the
// first half has the same outcome with the cache and without.
TEST_P(SchemaCreateCacheDdlTest, SchemaChangeMatchesUncached) {
  const DdlCase& ddl = GetParam();
  if (ddl.statements.size() < 2) GTEST_SKIP();
  const size_t half = ddl.statements.size() / 2;
  DdlCase first = ddl;
  first.statements.assign(ddl.statements.begin(),
                          ddl.statements.begin() + half);
  const std::vector<std::string> second(ddl.statements.begin() + half,
                                        ddl.statements.end());
  auto [cached, reference] = CreateBoth("m", first);
  ExpectSame(cached, reference, ddl.dialect);
  if (reference.code != absl::StatusCode::kOk) return;

  absl::Status reference_status =
      Update(/*reference=*/true, "m", second, ddl.dialect);
  absl::Status cached_status =
      Update(/*reference=*/false, "m", second, ddl.dialect);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      Observed reference_after,
      Observe(/*reference=*/true, "m", ddl.dialect, absl::OkStatus()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      Observed cached_after,
      Observe(/*reference=*/false, "m", ddl.dialect, absl::OkStatus()));
  EXPECT_EQ(cached_status, reference_status);
  ExpectSame(cached_after, reference_after, ddl.dialect);
}

INSTANTIATE_TEST_SUITE_P(
    Corpus, SchemaCreateCacheDdlTest, ::testing::ValuesIn(AllDdlCases()),
    [](const ::testing::TestParamInfo<DdlCase>& info) {
      return info.param.name;
    });

const DdlCase& SequenceDdl() {
  static const auto* ddl = new DdlCase{
      "sequences",
      DatabaseDialect::GOOGLE_STANDARD_SQL,
      {"CREATE SEQUENCE seq OPTIONS (sequence_kind = 'bit_reversed_positive')",
       R"(CREATE TABLE t (
            id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)),
            ident INT64 GENERATED BY DEFAULT AS IDENTITY (BIT_REVERSED_POSITIVE),
            v INT64,
          ) PRIMARY KEY (id))",
       "CREATE CHANGE STREAM cs FOR t"}};
  return *ddl;
}

// The keys a database's sequences and identity column give its first rows.
absl::StatusOr<std::vector<std::string>> InsertRows(
    cloud::spanner::Client& client, int rows) {
  std::vector<std::string> keys;
  for (int i = 0; i < rows; ++i) {
    std::string key;
    auto result = client.Commit(
        [&](cloud::spanner::Transaction txn)
            -> google::cloud::StatusOr<cloud::spanner::Mutations> {
          key.clear();
          auto stream = client.ExecuteQuery(
              txn, cloud::spanner::SqlStatement(absl::StrCat(
                       "INSERT INTO t (v) VALUES (", i,
                       ") THEN RETURN id, ident")));
          for (auto& row :
               cloud::spanner::StreamOf<std::tuple<int64_t, int64_t>>(
                   stream)) {
            if (!row) return row.status();
            key = absl::StrCat(std::get<0>(*row), "/", std::get<1>(*row));
          }
          return cloud::spanner::Mutations{};
        });
    if (!result) return absl::InternalError(result.status().message());
    keys.push_back(key);
  }
  return keys;
}

// Databases created from the cache give their rows the keys a new database
// gives, and draw from their sequences independently, concurrently.
TEST_F(SchemaCreateCacheTest, SequencesAreIndependent) {
  constexpr int kDatabases = 8;
  constexpr int kRows = 10;
  GOOGLESQL_ASSERT_OK(Create(/*reference=*/true, "s", SequenceDdl()));
  auto reference_client = MakeClient(/*reference=*/true, "s");
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> expected,
                       InsertRows(*reference_client, kRows));

  for (int i = 0; i < kDatabases; ++i) {
    GOOGLESQL_ASSERT_OK(Create(/*reference=*/false, absl::StrCat("s", i), SequenceDdl()));
  }
  EXPECT_EQ(cache_.hits(), kDatabases - 1);
  std::vector<std::thread> threads;
  for (int i = 0; i < kDatabases; ++i) {
    threads.emplace_back([&, i] {
      auto client = MakeClient(/*reference=*/false, absl::StrCat("s", i));
      EXPECT_THAT(InsertRows(*client, kRows),
                  googlesql_base::testing::IsOkAndHolds(
                      testing::ElementsAreArray(expected)));
    });
  }
  for (std::thread& thread : threads) thread.join();
}

// A database created from the cache has its own change stream partitions.
TEST_F(SchemaCreateCacheTest, ChangeStreamPartitions) {
  GOOGLESQL_ASSERT_OK(Create(/*reference=*/true, "c", SequenceDdl()));
  GOOGLESQL_ASSERT_OK(Create(/*reference=*/false, "cw", SequenceDdl()));
  GOOGLESQL_ASSERT_OK(Create(/*reference=*/false, "c", SequenceDdl()));
  EXPECT_EQ(cache_.hits(), 1);
  std::vector<std::vector<std::string>> tokens;
  for (bool reference : {true, false}) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::string session,
        CreateSession(absl::StrCat(InstanceUri(reference), "/databases/",
                                   DbId("c"))));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        tokens.emplace_back(),
        GetActiveTokenFromInitialQuery(SequenceDdl().dialect, absl::Now(),
                                       "cs", session, raw_client()));
  }
  // The initial partitions; their tokens are random.
  EXPECT_EQ(tokens[1].size(), tokens[0].size());
  EXPECT_FALSE(tokens[1].empty());
  EXPECT_NE(tokens[1], tokens[0]);
}

// Concurrent creates of one DDL list, racing to fill the cache, all create
// the same database.
TEST_F(SchemaCreateCacheTest, ConcurrentCreatesOfTheSameDdl) {
  GOOGLESQL_ASSERT_OK(Create(/*reference=*/true, "r", SequenceDdl()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      Observed expected,
      Observe(/*reference=*/true, "r", SequenceDdl().dialect,
              absl::OkStatus()));
  constexpr int kThreads = 16;
  std::vector<std::thread> threads;
  std::vector<absl::Status> statuses(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      statuses[i] = CreateUsingCurrentCache(/*reference=*/false,
                                            absl::StrCat("r", i), SequenceDdl());
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (int i = 0; i < kThreads; ++i) {
    GOOGLESQL_ASSERT_OK(statuses[i]);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        Observed observed,
        Observe(/*reference=*/false, absl::StrCat("r", i),
                SequenceDdl().dialect, absl::OkStatus()));
    EXPECT_THAT(observed.ddl, testing::ElementsAreArray(expected.ddl));
  }
}

// After its entry is evicted, a database created from it keeps working, and
// the same request is created from DDL again.
TEST_F(SchemaCreateCacheTest, CreateAfterEviction) {
  SchemaCreateCache small(1);
  SchemaCreateCache::SetGlobalForTesting(&small);
  DdlCase other = SequenceDdl();
  other.statements.pop_back();
  GOOGLESQL_ASSERT_OK(CreateUsingCurrentCache(false, "e1", SequenceDdl()));
  GOOGLESQL_ASSERT_OK(CreateUsingCurrentCache(false, "e2", SequenceDdl()));
  EXPECT_EQ(small.hits(), 1);
  GOOGLESQL_ASSERT_OK(CreateUsingCurrentCache(false, "e3", other));
  EXPECT_EQ(small.size(), 1);
  GOOGLESQL_ASSERT_OK(CreateUsingCurrentCache(false, "e4", SequenceDdl()));
  EXPECT_EQ(small.hits(), 1);
  SchemaCreateCache::SetGlobalForTesting(&cache_);

  GOOGLESQL_ASSERT_OK(Create(/*reference=*/true, "e2", SequenceDdl()));
  for (const char* id : {"e2", "e4"}) {
    GOOGLESQL_ASSERT_OK(
        Update(false, id, {"ALTER TABLE t ADD COLUMN a ARRAY<INT64>",
                           "CREATE INDEX t_by_v ON t(v)"}));
  }
  GOOGLESQL_ASSERT_OK(Update(true, "e2", {"ALTER TABLE t ADD COLUMN a ARRAY<INT64>",
                                "CREATE INDEX t_by_v ON t(v)"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      Observed reference,
      Observe(true, "e2", SequenceDdl().dialect, absl::OkStatus()));
  for (const char* id : {"e2", "e4"}) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        Observed observed,
        Observe(false, id, SequenceDdl().dialect, absl::OkStatus()));
    ExpectSame(observed, reference, SequenceDdl().dialect);
  }
}

}  // namespace
}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
