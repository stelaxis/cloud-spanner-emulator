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

#include <algorithm>
#include <atomic>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdint>
#include <memory>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "google/longrunning/operations.pb.h"
#include "google/protobuf/any.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "google/spanner/v1/commit_response.pb.h"
#include "google/protobuf/descriptor.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/database/database.h"
#include "backend/database/schema_create_cache.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/printer/print_ddl.h"
#include "common/constants.h"
#include "common/errors.h"
#include "common/limits.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/multiplexed_session_transaction_manager.h"
#include "frontend/common/protos.h"
#include "frontend/common/uris.h"
#include "frontend/entities/database.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

using google::spanner::emulator::backend::Sequence;
using googlesql_base::testing::IsOkAndHolds;
using googlesql_base::testing::StatusIs;

namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;
namespace spanner_api = ::google::spanner::v1;

class DatabaseApiTest : public test::ServerTest {
 protected:
  void SetUp() override { GOOGLESQL_EXPECT_OK(CreateTestInstance()); }
  void TearDown() override { GOOGLESQL_EXPECT_OK(CleanupTestInstance()); }

  // Generate proto descriptor bytes as string
  std::string GenerateProtoDescriptorBytesAsString() {
    const google::protobuf::FileDescriptorProto file_descriptor = PARSE_TEXT_PROTO(R"pb(
      syntax: "proto2"
      name: "0"
      package: "customer.app"
      message_type { name: "User" }
      enum_type {
        name: "State"
        value { name: "UNSPECIFIED" number: 0 }
      }
    )pb");
    google::protobuf::FileDescriptorSet file_descriptor_set;
    *file_descriptor_set.add_file() = file_descriptor;
    return file_descriptor_set.SerializeAsString();
  }

  absl::Status ListDatabases(const std::string& instance_uri, int32_t page_size,
                             const std::string& page_token,
                             database_api::ListDatabasesResponse* response) {
    grpc::ClientContext context;
    database_api::ListDatabasesRequest request;
    request.set_parent(instance_uri);
    request.set_page_size(page_size);
    request.set_page_token(page_token);
    return test_env()->database_admin_client()->ListDatabases(&context, request,
                                                              response);
  }

  absl::Status CreateDatabase(
      const std::string& instance_uri, const std::string& database_name,
      const std::vector<std::string>& extra_statements = {},
      const database_api::DatabaseDialect& dialect =
          // DATABASE_DIALECT_UNSPECIFIED defaults to creating a database with
          // the GOOGLE_STANDARD_SQL dialect.
      database_api::DatabaseDialect::DATABASE_DIALECT_UNSPECIFIED) {
    grpc::ClientContext context;
    database_api::CreateDatabaseRequest request;
    request.set_parent(instance_uri);
    std::string quote =
        (dialect == database_api::POSTGRESQL) ? kPGQuote : kGSQLQuote;
    request.set_create_statement(
        absl::StrCat("CREATE DATABASE ", quote, database_name, quote));
    for (auto extra_statement : extra_statements) {
      request.add_extra_statements(extra_statement);
    }
    request.set_proto_descriptors(GenerateProtoDescriptorBytesAsString());
    request.set_database_dialect(dialect);
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->database_admin_client()->CreateDatabase(
        &context, request, &operation));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    database_api::CreateDatabaseMetadata metadata;
    GOOGLESQL_RET_CHECK(operation.metadata().UnpackTo(&metadata));
    GOOGLESQL_RET_CHECK_EQ(metadata.database(),
                 MakeDatabaseUri(instance_uri, database_name));
    return absl::OkStatus();
  }

  absl::Status GetDatabase(const std::string& database_uri,
                           database_api::Database* database) {
    grpc::ClientContext context;
    database_api::GetDatabaseRequest request;
    request.set_name(database_uri);
    return test_env()->database_admin_client()->GetDatabase(&context, request,
                                                            database);
  }

  absl::Status GetDatabaseDdl(const std::string& database_uri,
                              database_api::GetDatabaseDdlResponse* response) {
    grpc::ClientContext context;
    database_api::GetDatabaseDdlRequest request;
    request.set_database(database_uri);
    GOOGLESQL_RETURN_IF_ERROR(test_env()->database_admin_client()->GetDatabaseDdl(
        &context, request, response));
    return absl::OkStatus();
  }

  absl::Status DropDatabase(const std::string& database_uri) {
    grpc::ClientContext context;
    database_api::DropDatabaseRequest request;
    request.set_database(database_uri);
    protobuf_api::Empty response;
    return test_env()->database_admin_client()->DropDatabase(&context, request,
                                                             &response);
  }

 private:
  absl::Status CleanupTestInstance() {
    database_api::ListDatabasesResponse response;
    GOOGLESQL_RETURN_IF_ERROR(ListDatabases(test_instance_uri_, 0 /*page_size*/,
                                  "" /*page_token*/, &response));
    while (!response.databases().empty()) {
      for (const auto& database : response.databases()) {
        GOOGLESQL_RETURN_IF_ERROR(DropDatabase(database.name()));
      }
      response.clear_databases();
      if (!response.next_page_token().empty()) {
        std::string next_page_token = response.next_page_token();
        response.clear_next_page_token();
        GOOGLESQL_RETURN_IF_ERROR(ListDatabases(test_instance_uri_, 0 /*page_size*/,
                                      next_page_token, &response));
      }
    }
    return absl::OkStatus();
  }
};

// Tests for CreateDatabase.

TEST_F(DatabaseApiTest, CreateDatabaseWithInvalidDatabaseName) {
  // Name less than 2 characters.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "a"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Name greater than 30 characters.
  EXPECT_THAT(CreateDatabase(test_instance_uri_,
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Only lowercase allowed.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "AAAAA"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Non-alphanumeric characters are not allowed.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "aaaa!@#$aaa"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Cannot end with hypen.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, "aaaa-aaaa-"),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(DatabaseApiTest, LimitDatabasePerInstance) {
  for (int i = 0; i < limits::kMaxDatabasesPerInstance; ++i) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                             absl::StrCat(test_database_name_, i)));
  }
  // Creating the next database fails.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST_F(DatabaseApiTest, OverrideMaxDatabasePerInstanceLimit) {
  int custom_max_dbs_per_instance = 150;
  absl::SetFlag(&FLAGS_override_max_databases_per_instance,
                custom_max_dbs_per_instance);
  for (int i = 0; i < custom_max_dbs_per_instance; ++i) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                             absl::StrCat(test_database_name_, i)));
  }
  // Creating the next database fails.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kResourceExhausted));
}

TEST_F(DatabaseApiTest, CreateDatabaseEmptyInitialSchema) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithInitialSchema) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                           {
                               R"(
                                 CREATE TABLE test_table (
                                   int64_col INT64 NOT NULL,
                                   string_col STRING(MAX)
                                 ) PRIMARY KEY(int64_col)
                               )",
                           }));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithInvalidInitialSchema) {
  EXPECT_THAT(
      CreateDatabase(test_instance_uri_, test_database_name_, {"INVALID DDL"}),
      StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(DatabaseApiTest, CreateDuplicateDatabaseReturnsAlreadyExists) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  // Request to create same database.
  EXPECT_THAT(CreateDatabase(test_instance_uri_, test_database_name_),
              StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithoutInstanceReturnsNotFound) {
  EXPECT_THAT(CreateDatabase(
                  MakeInstanceUri(test_project_name_, "non-existent-instance"),
                  test_database_name_),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithGSQLDialect) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                           absl::StrCat(test_database_name_, "_gsql"),
                           /* extra_statements = */ {},
                           database_api::DatabaseDialect::GOOGLE_STANDARD_SQL));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithPostgresDialectDisabledByDefault) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_pg_1"),
      /* extra_statements = */ {}, database_api::DatabaseDialect::POSTGRESQL));
}

TEST_F(DatabaseApiTest, CreateDatabaseWithPostgresDialect) {
  test::ScopedEmulatorFeatureFlagsSetter enabled_flags(
      {.enable_postgresql_interface = true});
  GOOGLESQL_EXPECT_OK(CreateDatabase(
      test_instance_uri_, absl::StrCat(test_database_name_, "_pg_1"),
      /* extra_statements = */ {}, database_api::DatabaseDialect::POSTGRESQL));

  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                           absl::StrCat(test_database_name_, "_pg_2"),
                           {
                               R"(
                                   CREATE TABLE test_table (
                                     int64_col bigint not null primary key,
                                     string_col varchar(1024)
                                   )
                                 )",
                           },
                           database_api::DatabaseDialect::POSTGRESQL));
}

// Tests for ListDatabases.
TEST_F(DatabaseApiTest, DoesNotListDatabasesForUnknownInstance) {
  database_api::ListDatabasesResponse response;
  EXPECT_THAT(ListDatabases(
                  MakeInstanceUri(test_project_name_, "non-existent-instance"),
                  0 /*page_size*/, "" /*page_token*/, &response),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, ListsEmptyDatabasesForNewInstance) {
  database_api::ListDatabasesResponse response;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, 0 /*page_size*/,
                          "" /*page_token*/, &response));
  EXPECT_EQ(response.databases_size(), 0);
}

TEST_F(DatabaseApiTest, ListsCustomPageSizeDatabases) {
  // Create 5+3 = 8 databases test-database0, test-database1,...,
  // test-database7 in test-instance.
  int32_t page_size = 5;
  int32_t last_page_size = 3;
  for (int i = 0; i < page_size + last_page_size; i++) {
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_,
                             absl::StrCat(test_database_name_, i)));
  }

  // List databases from test-instance with page_size being 5, i.e., only 5
  // databases test-database0, test-database1,..., test-database4 should be
  // returned with next_page_token pointing to test-database5.
  database_api::ListDatabasesResponse response;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, page_size, "" /*page_token*/,
                          &response));
  EXPECT_EQ(response.databases_size(), page_size);
  for (int i = 0; i < page_size; i++) {
    EXPECT_EQ(response.databases(i).name(),
              absl::StrCat(test_database_uri_, i));
  }
  EXPECT_EQ(response.next_page_token(),
            absl::StrCat(test_database_uri_, page_size));

  // Using the next_page_token pointing to test-database5, list next at most 5
  // databases test-database5, test-database6 and test-database7.
  database_api::ListDatabasesResponse response2;
  GOOGLESQL_EXPECT_OK(ListDatabases(test_instance_uri_, page_size,
                          response.next_page_token(), &response2));
  EXPECT_EQ(response2.databases_size(), last_page_size);
  for (int i = 0; i < last_page_size; i++) {
    EXPECT_EQ(response2.databases(i).name(),
              absl::StrCat(test_database_uri_, i + page_size));
  }
  // No more databases left to be returned and thus next_page_token is not set.
  EXPECT_EQ(response2.next_page_token(), "");
}

// Tests for UpdateDatabaseDdl.

TEST_F(DatabaseApiTest, UpdateDatabaseDdlInvalidDatabaseUri) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  // Invalid project name should return an error.
  auto instance_uri =
      MakeInstanceUri("non-existent-project", test_instance_name_);
  EXPECT_THAT(
      UpdateDatabaseDdl(MakeDatabaseUri(instance_uri, test_database_name_), {}),
      StatusIs(absl::StatusCode::kNotFound));

  // Invalid instance name should return an error.
  instance_uri = MakeInstanceUri(test_project_name_, "non-existent-instance");
  EXPECT_THAT(
      UpdateDatabaseDdl(MakeDatabaseUri(instance_uri, test_database_name_), {}),
      StatusIs(absl::StatusCode::kNotFound));

  // Invalid database name should return an error.
  EXPECT_THAT(
      UpdateDatabaseDdl(
          MakeDatabaseUri(test_instance_uri_, "non-existent-database"), {}),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, UpdateDatabaseDdlPartialSuccess) {
  GOOGLESQL_EXPECT_OK(CreateTestDatabase());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                       CreateTestSession(/*multiplexed=*/false));

  spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      insert {
        table: "test_table"
        columns: "int64_col"
        columns: "string_col"
        values {
          values { string_value: "1" }
          values { string_value: "a" }
        }
        values {
          values { string_value: "2" }
          values { string_value: "a" }
        }
      }
    }
  )pb");
  *commit_request.mutable_session() = session;
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(Commit(commit_request, &commit_response));

  database_api::UpdateDatabaseDdlMetadata metadata;
  std::vector<std::string> statements = {R"(
     CREATE TABLE another_table (
       int64_col INT64 NOT NULL,
     ) PRIMARY KEY (int64_col)
  )",
                                         R"(
     CREATE UNIQUE INDEX test_index ON test_table(string_col)
  )"};

  EXPECT_THAT(UpdateDatabaseDdl(test_database_uri_, statements, &metadata),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr(
                           "Found uniqueness violation on index test_index")));

  EXPECT_EQ(metadata.commit_timestamps_size(), 1);
  EXPECT_EQ(metadata.statements_size(), 2);
  for (int i = 0; i < metadata.statements_size(); ++i) {
    EXPECT_EQ(metadata.statements(i), statements[i]);
  }
}

TEST_F(DatabaseApiTest, GetDatabaseNonExistentDatabase) {
  database_api::Database database;
  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, GetDatabase) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseApiTest, GetDatabaseWithGSQLDialect) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_, {},
                           database_api::DatabaseDialect::GOOGLE_STANDARD_SQL));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::GOOGLE_STANDARD_SQL);
}

TEST_F(DatabaseApiTest, GetDatabaseWithPostgresDialect) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_, {},
                           database_api::DatabaseDialect::POSTGRESQL));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  EXPECT_EQ(database.name(), test_database_uri_);
  EXPECT_EQ(database.database_dialect(),
            database_api::DatabaseDialect::POSTGRESQL);
}

TEST_F(DatabaseApiTest, DropDatabaseInvalidInstance) {
  auto instance_uri =
      MakeInstanceUri(test_project_name_, "invalid-instance-name");
  EXPECT_THAT(DropDatabase(MakeDatabaseUri(instance_uri, test_database_name_)),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, DropDatabase) {
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));

  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));

  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));

  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DatabaseApiTest, DropDatabaseIdempotent) {
  database_api::Database database;
  EXPECT_THAT(GetDatabase(test_database_uri_, &database),
              StatusIs(absl::StatusCode::kNotFound));

  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
}

TEST_F(DatabaseApiTest, UpdateAndGetDatabaseDDL) {
  std::vector<std::vector<std::string>> test_schemas = {
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
  ts TIMESTAMP OPTIONS (
    allow_commit_timestamp = true
  ),
) PRIMARY KEY(int64_col))"},
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
) PRIMARY KEY(int64_col))",
          R"(CREATE UNIQUE NULL_FILTERED INDEX test_index ON test_table(string_col))",
      },
      {
          R"(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
) PRIMARY KEY(int64_col))"},
      {
          R"(CREATE TABLE test_table (
) PRIMARY KEY())"},
      {
          R"sql(CREATE PROTO BUNDLE (
  customer.app.State,
  customer.app.User,
))sql",
          R"sql(CREATE TABLE test_table (
  int64_col INT64 NOT NULL,
  string_col STRING(MAX),
  user_col `customer.app.User`,
  state_col `customer.app.State`,
) PRIMARY KEY(int64_col))sql"},
  };

  for (int i = 0; i < test_schemas.size(); ++i) {
    auto schema = test_schemas[i];
    GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_, schema));

    database_api::GetDatabaseDdlResponse response;
    GOOGLESQL_EXPECT_OK(GetDatabaseDdl(test_database_uri_, &response));

    for (int j = 0; j < schema.size(); ++j) {
      EXPECT_THAT(response.statements(j), schema[j]);
    }
    EXPECT_EQ(response.proto_descriptors(),
              i == test_schemas.size() - 1
                  ? GenerateProtoDescriptorBytesAsString()
                  : "");

    GOOGLESQL_EXPECT_OK(
        DropDatabase(MakeDatabaseUri(test_instance_uri_, test_database_name_)));
  }
}

spanner_api::CommitRequest GenerateSequenceTableInsert(
    const std::string& session) {
  spanner_api::CommitRequest commit_request = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      insert {
        table: "test_table"
        columns: "col"
        values { values { string_value: "1" } }
        values { values { string_value: "2" } }
      }
    }
  )pb");
  *commit_request.mutable_session() = session;
  return commit_request;
}

TEST_F(DatabaseApiTest, CreateSameNameSequencesInTwoDatabases) {
  std::vector<std::string> schema = {
      R"(CREATE SEQUENCE test_sequence OPTIONS (
  sequence_kind = 'bit_reversed_positive'))",
      R"(CREATE TABLE test_table (
  id INT64 DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE test_sequence)),
  col INT64,
) PRIMARY KEY(id))"};
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, "test_db_1", schema));
  std::string database_uri_1 = MakeDatabaseUri(test_instance_uri_, "test_db_1");

  database_api::GetDatabaseDdlResponse response;
  GOOGLESQL_EXPECT_OK(GetDatabaseDdl(database_uri_1, &response));

  for (int i = 0; i < schema.size(); ++i) {
    EXPECT_THAT(response.statements(i), schema[i]);
  }

  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, "test_db_2", schema));
  std::string database_uri_2 = MakeDatabaseUri(test_instance_uri_, "test_db_2");
  GOOGLESQL_EXPECT_OK(GetDatabaseDdl(database_uri_2, &response));

  for (int i = 0; i < schema.size(); ++i) {
    EXPECT_THAT(response.statements(i), schema[i]);
  }

  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string session_1,
      CreateTestSession(/*multiplexed=*/false, database_uri_1));
  GOOGLESQL_ASSERT_OK(Commit(GenerateSequenceTableInsert(session_1), &commit_response));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      const std::string session_2,
      CreateTestSession(/*multiplexed=*/false, database_uri_2));
  GOOGLESQL_ASSERT_OK(Commit(GenerateSequenceTableInsert(session_2), &commit_response));

  {
    absl::MutexLock lock(Sequence::SequenceMutex);
    ASSERT_EQ(Sequence::SequenceLastValues.size(), 2);
  }
}

// Tests for what DropDatabase releases.

std::vector<std::string> SequenceTableSchema() {
  return {R"(CREATE SEQUENCE test_sequence OPTIONS (
  sequence_kind = 'bit_reversed_positive'))",
          R"(CREATE TABLE test_table (
  id INT64 DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE test_sequence)),
  col INT64,
) PRIMARY KEY(id))"};
}

size_t SequenceCounterCount() {
  absl::MutexLock lock(Sequence::SequenceMutex);
  return Sequence::SequenceLastValues.size();
}

// Pauses the first call of a hook until Resume. Later calls pass through.
class PausePoint {
 public:
  void MaybePause(int64_t arg) {
    if (paused_.exchange(true)) return;
    arg_ = arg;
    reached_.Notify();
    resume_.WaitForNotification();
  }
  // Returns the argument of the paused call.
  int64_t WaitUntilReached() {
    reached_.WaitForNotification();
    return arg_;
  }
  void Resume() { resume_.Notify(); }

 private:
  std::atomic<bool> paused_ = false;
  int64_t arg_ = 0;
  absl::Notification reached_;
  absl::Notification resume_;
};

class DropDatabaseTest : public DatabaseApiTest {
 protected:
  std::weak_ptr<Database> GetDatabaseWeakPtr(const std::string& database_uri) {
    absl::StatusOr<std::shared_ptr<Database>> database =
        test_env()->env()->database_manager()->GetDatabase(database_uri);
    if (!database.ok()) return {};
    return *database;
  }

  absl::Status Select1(const std::string& session) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql("SELECT 1");
    spanner_api::ResultSet result;
    return ExecuteSql(request, &result);
  }

  absl::StatusOr<std::vector<int64_t>> SelectIds(const std::string& session) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql("SELECT id FROM test_table ORDER BY col");
    spanner_api::ResultSet result;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &result));
    std::vector<int64_t> ids;
    for (const auto& row : result.rows()) {
      ids.push_back(std::stoll(row.values(0).string_value()));
    }
    return ids;
  }

  absl::Status UpdateDdlWithOperationId(const std::string& database_uri,
                                        const std::string& operation_id) {
    grpc::ClientContext context;
    database_api::UpdateDatabaseDdlRequest request;
    request.set_database(database_uri);
    request.set_operation_id(operation_id);
    request.add_statements("CREATE INDEX test_index ON test_table(col)");
    operations_api::Operation operation;
    return test_env()->database_admin_client()->UpdateDatabaseDdl(
        &context, request, &operation);
  }

  absl::StatusOr<int> CountOperations(const std::string& resource_uri) {
    grpc::ClientContext context;
    operations_api::ListOperationsRequest request;
    request.set_name(absl::StrCat(resource_uri, "/operations"));
    operations_api::ListOperationsResponse response;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->operations_client()->ListOperations(
        &context, request, &response));
    return response.operations_size();
  }
};

TEST_F(DropDatabaseTest, ReleasesDatabaseWithIdleSessions) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  std::weak_ptr<Database> database = GetDatabaseWeakPtr(test_database_uri_);
  ASSERT_FALSE(database.expired());
  GOOGLESQL_ASSERT_OK(CreateTestSession(/*multiplexed=*/false).status());
  // Past the one-hour expiration of idle sessions.
  test_env()->AdvanceClock(absl::Hours(2));
  GOOGLESQL_ASSERT_OK(CreateTestSession(/*multiplexed=*/false).status());
  GOOGLESQL_ASSERT_OK(CreateTestSession(/*multiplexed=*/true).status());

  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  EXPECT_TRUE(database.expired());
}

TEST_F(DropDatabaseTest, DeleteInstanceReleasesDatabaseWithIdleSessions) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  std::weak_ptr<Database> database = GetDatabaseWeakPtr(test_database_uri_);
  GOOGLESQL_ASSERT_OK(CreateTestSession(/*multiplexed=*/false).status());
  test_env()->AdvanceClock(absl::Hours(2));

  grpc::ClientContext context;
  instance_api::DeleteInstanceRequest request;
  request.set_name(test_instance_uri_);
  protobuf_api::Empty response;
  GOOGLESQL_ASSERT_OK(test_env()->instance_admin_client()->DeleteInstance(
      &context, request, &response));

  EXPECT_TRUE(database.expired());
  // For TearDown.
  GOOGLESQL_ASSERT_OK(CreateTestInstance());
}

TEST_F(DropDatabaseTest, SessionsOfDroppedDatabaseFailAsUpstream) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string idle_session,
                                 CreateTestSession(/*multiplexed=*/false));
  test_env()->AdvanceClock(absl::Hours(2));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string recent_session,
                                 CreateTestSession(/*multiplexed=*/false));
  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  // A session's database is looked up before the session itself.
  const absl::Status database_not_found =
      error::DatabaseNotFound(test_database_uri_);
  for (const std::string& session : {idle_session, recent_session}) {
    EXPECT_THAT(Select1(session),
                StatusIs(database_not_found.code(),
                         std::string(database_not_found.message())));
  }

  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  for (const std::string& session : {idle_session, recent_session}) {
    const absl::Status session_not_found = error::SessionNotFound(session);
    EXPECT_THAT(Select1(session),
                StatusIs(session_not_found.code(),
                         std::string(session_not_found.message())));
  }
}

TEST_F(DropDatabaseTest, ReleasesSequenceCounters) {
  const size_t counters = SequenceCounterCount();
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                                 CreateTestSession(/*multiplexed=*/false));
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(
      Commit(GenerateSequenceTableInsert(session), &commit_response));
  ASSERT_EQ(SequenceCounterCount(), counters + 1);

  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  EXPECT_EQ(SequenceCounterCount(), counters);
}

TEST_F(DropDatabaseTest, DeletesOperations) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  GOOGLESQL_ASSERT_OK(UpdateDdlWithOperationId(test_database_uri_, "ddl1"));
  EXPECT_THAT(CountOperations(test_database_uri_), IsOkAndHolds(2));

  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  EXPECT_THAT(CountOperations(test_database_uri_), IsOkAndHolds(0));
  operations_api::Operation operation;
  EXPECT_THAT(
      GetOperation(MakeOperationUri(test_database_uri_, "ddl1"), &operation),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DropDatabaseTest, RemovesMultiplexedSessionTransactions) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  std::weak_ptr<Database> database = GetDatabaseWeakPtr(test_database_uri_);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                                 CreateTestSession(/*multiplexed=*/true));
  spanner_api::BeginTransactionRequest request;
  request.set_session(session);
  request.mutable_options()->mutable_read_write();
  spanner_api::Transaction transaction;
  GOOGLESQL_ASSERT_OK(BeginTransaction(request, &transaction));
  const backend::TransactionID id = TransactionIDFromProto(transaction.id());
  GOOGLESQL_ASSERT_OK(
      test_env()
          ->env()
          ->mux_txn_manager()
          ->GetCurrentTransactionOnMultiplexedSession(*database.lock(), id));

  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  // The transaction's entry owned the database.
  EXPECT_TRUE(database.expired());
}

TEST_F(DropDatabaseTest, RecreatedDatabaseStartsClean) {
  const size_t counters = SequenceCounterCount();
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string session,
                                 CreateTestSession(/*multiplexed=*/false));
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(
      Commit(GenerateSequenceTableInsert(session), &commit_response));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<int64_t> first_ids,
                                 SelectIds(session));
  ASSERT_EQ(first_ids.size(), 2);
  GOOGLESQL_ASSERT_OK(UpdateDdlWithOperationId(test_database_uri_, "ddl1"));
  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  EXPECT_EQ(SequenceCounterCount(), counters);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(session,
                                 CreateTestSession(/*multiplexed=*/false));
  EXPECT_THAT(SelectIds(session), IsOkAndHolds(testing::IsEmpty()));
  GOOGLESQL_ASSERT_OK(
      Commit(GenerateSequenceTableInsert(session), &commit_response));
  EXPECT_THAT(SelectIds(session), IsOkAndHolds(first_ids));
  // The dropped database's operation ids are free again.
  GOOGLESQL_EXPECT_OK(UpdateDdlWithOperationId(test_database_uri_, "ddl1"));
}

TEST_F(DropDatabaseTest, MultiplexedTransactionBegunDuringDropIsNotPublished) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  std::weak_ptr<Database> database = GetDatabaseWeakPtr(test_database_uri_);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                                 CreateTestSession(/*multiplexed=*/true));
  auto pause = std::make_shared<PausePoint>();
  test_env()->env()->mux_txn_manager()->set_before_add_hook_for_testing(
      [pause](backend::TransactionID id) { pause->MaybePause(id); });

  // The transaction is created, then pauses before it is published.
  absl::Status begin_status;
  std::thread begin([&] {
    spanner_api::BeginTransactionRequest request;
    request.set_session(session);
    request.mutable_options()->mutable_read_write();
    spanner_api::Transaction transaction;
    begin_status = BeginTransaction(request, &transaction);
  });
  const backend::TransactionID id = pause->WaitUntilReached();
  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
  pause->Resume();
  begin.join();

  EXPECT_THAT(begin_status, StatusIs(absl::StatusCode::kNotFound));
  EXPECT_TRUE(database.expired());
  // The old id reaches nothing in a database re-created under the same name.
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string new_session,
                                 CreateTestSession(/*multiplexed=*/true));
  spanner_api::ExecuteSqlRequest request;
  request.set_session(new_session);
  request.set_sql("SELECT id FROM test_table");
  request.mutable_transaction()->set_id(absl::StrCat(id));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  spanner_api::ResultSet result;
  EXPECT_THAT(
      test_env()->spanner_client()->ExecuteSql(&context, request, &result),
      StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(DropDatabaseTest, StaleDropLeavesRecreatedDatabaseAlone) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  auto pause = std::make_shared<PausePoint>();
  test_env()->env()->set_drop_database_hook_for_testing(
      [pause] { pause->MaybePause(0); });

  // The first drop pauses after picking the database; a second drop then
  // completes and the database is re-created with a session.
  absl::Status stale_drop_status;
  std::thread stale_drop(
      [&] { stale_drop_status = DropDatabase(test_database_uri_); });
  pause->WaitUntilReached();
  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
  GOOGLESQL_EXPECT_OK(CreateDatabase(test_instance_uri_, test_database_name_));
  absl::StatusOr<std::string> session =
      CreateTestSession(/*multiplexed=*/false);
  GOOGLESQL_EXPECT_OK(session.status());
  pause->Resume();
  stale_drop.join();

  GOOGLESQL_EXPECT_OK(stale_drop_status);
  database_api::Database database;
  GOOGLESQL_EXPECT_OK(GetDatabase(test_database_uri_, &database));
  if (session.ok()) {
    GOOGLESQL_EXPECT_OK(Select1(*session));
  }
}

TEST_F(DropDatabaseTest, DdlOperationFinishingAfterDropIsNotPublished) {
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  auto pause = std::make_shared<PausePoint>();
  test_env()->env()->operation_manager()->set_before_create_hook_for_testing(
      [pause] { pause->MaybePause(0); });

  // The schema change is applied, then pauses before its operation is
  // published.
  absl::Status ddl_status;
  std::thread ddl([&] {
    ddl_status = UpdateDdlWithOperationId(test_database_uri_, "ddl1");
  });
  pause->WaitUntilReached();
  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
  pause->Resume();
  ddl.join();

  EXPECT_THAT(ddl_status, StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(CountOperations(test_database_uri_), IsOkAndHolds(0));
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, test_database_name_,
                                     SequenceTableSchema()));
  GOOGLESQL_EXPECT_OK(UpdateDdlWithOperationId(test_database_uri_, "ddl1"));
}

TEST_F(DropDatabaseTest, CreateOperationFinishingAfterDropIsNotPublished) {
  auto pause = std::make_shared<PausePoint>();
  test_env()->env()->operation_manager()->set_before_create_hook_for_testing(
      [pause] { pause->MaybePause(0); });

  // The database is registered, then its creation pauses before the
  // operation is published.
  absl::Status create_status;
  std::thread create([&] {
    create_status = CreateDatabase(test_instance_uri_, test_database_name_);
  });
  pause->WaitUntilReached();
  GOOGLESQL_EXPECT_OK(DropDatabase(test_database_uri_));
  pause->Resume();
  create.join();

  EXPECT_THAT(create_status, StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(CountOperations(test_database_uri_), IsOkAndHolds(0));
}

TEST_F(DropDatabaseTest, ReleasesSequenceCountersOfFailedSchemaChanges) {
  const size_t counters = SequenceCounterCount();
  GOOGLESQL_ASSERT_OK(CreateDatabase(
      test_instance_uri_, test_database_name_,
      {"CREATE TABLE t (k INT64 NOT NULL, v INT64) PRIMARY KEY(k)"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session,
                                 CreateTestSession(/*multiplexed=*/false));
  spanner_api::CommitRequest commit = PARSE_TEXT_PROTO(R"pb(
    single_use_transaction { read_write {} }
    mutations {
      insert {
        table: "t"
        columns: "k"
        values { values { string_value: "1" } }
      }
    }
  )pb");
  commit.set_session(session);
  spanner_api::CommitResponse commit_response;
  GOOGLESQL_ASSERT_OK(Commit(commit, &commit_response));

  // Backfilling the existing row draws from the new identity sequence, which
  // is exhausted at once, so the schema change fails after its counter
  // exists.
  EXPECT_THAT(UpdateDatabaseDdl(
                  test_database_uri_,
                  {"ALTER TABLE t ADD COLUMN c INT64 GENERATED BY DEFAULT AS "
                   "IDENTITY (BIT_REVERSED_POSITIVE START COUNTER WITH "
                   "9223372036854775807)"}),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  ASSERT_EQ(SequenceCounterCount(), counters + 1);

  GOOGLESQL_ASSERT_OK(DropDatabase(test_database_uri_));

  EXPECT_EQ(SequenceCounterCount(), counters);
}


// Databases created from a copy of the schema cache's entry get their own
// sequence counters: each draws its own values, dropping one releases exactly
// its counters, and the entry keeps nothing of a dropped database.
TEST_F(DropDatabaseTest, CachedCopiesHaveTheirOwnSequenceCounters) {
  backend::SchemaCreateCache cache(4);
  // Restores the process-wide cache even if an assertion ends the test early.
  class ScopedGlobalCache {
   public:
    explicit ScopedGlobalCache(backend::SchemaCreateCache* cache)
        : previous_(backend::SchemaCreateCache::SetGlobalForTesting(cache)) {}
    ~ScopedGlobalCache() {
      backend::SchemaCreateCache::SetGlobalForTesting(previous_);
    }

   private:
    backend::SchemaCreateCache* const previous_;
  } scoped_cache(&cache);
  std::vector<std::string> schema = SequenceTableSchema();
  schema.push_back(R"(CREATE TABLE identity_table (
  id INT64 GENERATED BY DEFAULT AS IDENTITY (BIT_REVERSED_POSITIVE),
  col INT64,
) PRIMARY KEY(id))");
  const size_t counters = SequenceCounterCount();

  // The first create fills the cache; the next two are copies of its entry.
  GOOGLESQL_ASSERT_OK(
      CreateDatabase(test_instance_uri_, "cached_warm", schema));
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, "cached_a", schema));
  GOOGLESQL_ASSERT_OK(CreateDatabase(test_instance_uri_, "cached_b", schema));
  EXPECT_EQ(cache.copies(), 2);
  EXPECT_EQ(cache.fallbacks(), 0);
  const std::string uri_a = MakeDatabaseUri(test_instance_uri_, "cached_a");
  const std::string uri_b = MakeDatabaseUri(test_instance_uri_, "cached_b");
  std::weak_ptr<Database> database_a = GetDatabaseWeakPtr(uri_a);
  ASSERT_FALSE(database_a.expired());

  // Draws two values from the sequence and two from the identity column, and
  // returns every "table:id" in both tables. The two counters start alike, so
  // the same ids appear in both tables.
  auto draw = [&](const std::string& session)
      -> absl::StatusOr<std::vector<std::string>> {
    spanner_api::CommitResponse response;
    GOOGLESQL_RETURN_IF_ERROR(
        Commit(GenerateSequenceTableInsert(session), &response));
    spanner_api::CommitRequest identity_insert =
        GenerateSequenceTableInsert(session);
    identity_insert.mutable_mutations(0)->mutable_insert()->set_table(
        "identity_table");
    GOOGLESQL_RETURN_IF_ERROR(Commit(identity_insert, &response));
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql(
        "SELECT CONCAT('t:', CAST(id AS STRING)) FROM test_table UNION ALL "
        "SELECT CONCAT('i:', CAST(id AS STRING)) FROM identity_table");
    spanner_api::ResultSet result;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &result));
    std::vector<std::string> ids;
    for (const auto& row : result.rows()) {
      ids.push_back(row.values(0).string_value());
    }
    return ids;
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session_a,
                       CreateTestSession(/*multiplexed=*/false, uri_a));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(const std::string session_b,
                       CreateTestSession(/*multiplexed=*/false, uri_b));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> ids_a,
                       draw(session_a));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> ids_b,
                       draw(session_b));
  // Each copy starts its counters where a new database does.
  std::sort(ids_a.begin(), ids_a.end());
  std::sort(ids_b.begin(), ids_b.end());
  EXPECT_EQ(ids_a, ids_b);
  ASSERT_EQ(ids_a.size(), 4);
  // A counter per sequence per copy: the sequence and the identity column's.
  ASSERT_EQ(SequenceCounterCount(), counters + 4);

  GOOGLESQL_ASSERT_OK(DropDatabase(uri_a));

  EXPECT_TRUE(database_a.expired());
  EXPECT_EQ(SequenceCounterCount(), counters + 2);
  const absl::Status database_not_found = error::DatabaseNotFound(uri_a);
  EXPECT_THAT(Select1(session_a),
              StatusIs(database_not_found.code(),
                       std::string(database_not_found.message())));
  // The other copy's counters go on from where they were: reset counters would
  // draw its first ids again, which the inserts would refuse as duplicates.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> more_b,
                       draw(session_b));
  EXPECT_EQ(more_b.size(), 8);
  EXPECT_EQ(
      absl::flat_hash_set<std::string>(more_b.begin(), more_b.end()).size(), 8);

  GOOGLESQL_ASSERT_OK(DropDatabase(uri_b));
  GOOGLESQL_ASSERT_OK(
      DropDatabase(MakeDatabaseUri(test_instance_uri_, "cached_warm")));
  EXPECT_EQ(SequenceCounterCount(), counters);
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
