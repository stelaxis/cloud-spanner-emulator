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

// Ephemeral sessions (kEphemeralSessionLabel), through the gRPC API.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/sequence.h"
#include "common/config.h"
#include "common/errors.h"
#include "common/feature_flags.h"
#include "frontend/collections/session_manager.h"
#include "frontend/common/labels.h"
#include "frontend/common/uris.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"
#include "gmock/gmock.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/scoped_feature_flags_setter.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

namespace spanner_api = ::google::spanner::v1;
namespace database_api = ::google::spanner::admin::database::v1;
namespace operations_api = ::google::longrunning;
namespace protobuf_api = ::google::protobuf;

const Labels kEphemeral = {{kEphemeralSessionLabel, "true"}};

size_t SequenceCounterCount() {
  absl::MutexLock lock(backend::Sequence::SequenceMutex);
  return backend::Sequence::SequenceLastValues.size();
}

class EphemeralSessionTest : public test::ServerTest {
 protected:
  void SetUp() override {
    GOOGLESQL_ASSERT_OK(CreateTestInstance());
    GOOGLESQL_ASSERT_OK(CreateDatabase(
        {"CREATE TABLE T (k INT64 NOT NULL, v STRING(MAX)) PRIMARY KEY (k)"}));
  }

  void TearDown() override {
    config::set_max_ephemeral_sessions(256);
    config::set_ephemeral_session_idle_timeout(absl::Minutes(2));
  }

  absl::Status CreateDatabase(const std::vector<std::string>& statements) {
    grpc::ClientContext context;
    database_api::CreateDatabaseRequest request;
    request.set_parent(test_instance_uri_);
    request.set_create_statement(
        absl::StrCat("CREATE DATABASE `", test_database_name_, "`"));
    for (const std::string& statement : statements) {
      request.add_extra_statements(statement);
    }
    operations_api::Operation operation;
    GOOGLESQL_RETURN_IF_ERROR(
        test_env()->database_admin_client()->CreateDatabase(&context, request,
                                                            &operation));
    GOOGLESQL_RETURN_IF_ERROR(WaitForOperation(operation.name(), &operation));
    return absl::Status(static_cast<absl::StatusCode>(operation.error().code()),
                        operation.error().message());
  }

  absl::Status DropDatabase() {
    grpc::ClientContext context;
    database_api::DropDatabaseRequest request;
    request.set_database(test_database_uri_);
    protobuf_api::Empty response;
    return test_env()->database_admin_client()->DropDatabase(&context, request,
                                                             &response);
  }

  absl::StatusOr<std::vector<std::string>> GetDatabaseDdl() {
    grpc::ClientContext context;
    database_api::GetDatabaseDdlRequest request;
    request.set_database(test_database_uri_);
    database_api::GetDatabaseDdlResponse response;
    GOOGLESQL_RETURN_IF_ERROR(
        test_env()->database_admin_client()->GetDatabaseDdl(&context, request,
                                                            &response));
    return std::vector<std::string>(response.statements().begin(),
                                    response.statements().end());
  }

  absl::StatusOr<std::string> CreateSession(const Labels& labels,
                                            bool multiplexed = false) {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(test_database_uri_);
    request.mutable_session()->mutable_labels()->insert(labels.begin(),
                                                        labels.end());
    request.mutable_session()->set_multiplexed(multiplexed);
    spanner_api::Session response;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->spanner_client()->CreateSession(
        &context, request, &response));
    return response.name();
  }

  absl::StatusOr<std::vector<std::string>> BatchCreateSessions(
      const Labels& labels, int count) {
    grpc::ClientContext context;
    spanner_api::BatchCreateSessionsRequest request;
    request.set_database(test_database_uri_);
    request.mutable_session_template()->mutable_labels()->insert(labels.begin(),
                                                                 labels.end());
    request.set_session_count(count);
    spanner_api::BatchCreateSessionsResponse response;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->spanner_client()->BatchCreateSessions(
        &context, request, &response));
    std::vector<std::string> names;
    for (const spanner_api::Session& session : response.session()) {
      names.push_back(session.name());
    }
    return names;
  }

  absl::Status DeleteSession(const std::string& session) {
    grpc::ClientContext context;
    spanner_api::DeleteSessionRequest request;
    request.set_name(session);
    protobuf_api::Empty response;
    return test_env()->spanner_client()->DeleteSession(&context, request,
                                                       &response);
  }

  // Reads nothing, so it may follow AdvanceClock.
  absl::Status GetSession(const std::string& session) {
    grpc::ClientContext context;
    spanner_api::GetSessionRequest request;
    request.set_name(session);
    spanner_api::Session response;
    return test_env()->spanner_client()->GetSession(&context, request,
                                                    &response);
  }

  absl::StatusOr<std::vector<spanner_api::Session>> ListSessions() {
    grpc::ClientContext context;
    spanner_api::ListSessionsRequest request;
    request.set_database(test_database_uri_);
    spanner_api::ListSessionsResponse response;
    GOOGLESQL_RETURN_IF_ERROR(test_env()->spanner_client()->ListSessions(
        &context, request, &response));
    return std::vector<spanner_api::Session>(response.sessions().begin(),
                                             response.sessions().end());
  }

  // One string per row, values joined by commas.
  absl::StatusOr<std::vector<std::string>> Sql(const std::string& session,
                                               const std::string& sql) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session);
    request.set_sql(sql);
    spanner_api::ResultSet result;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &result));
    std::vector<std::string> rows;
    for (const auto& row : result.rows()) {
      std::vector<std::string> values;
      for (const auto& value : row.values()) {
        values.push_back(value.has_null_value() ? "NULL"
                                                : value.string_value());
      }
      rows.push_back(absl::StrJoin(values, ","));
    }
    return rows;
  }

  absl::StatusOr<std::vector<std::string>> Rows(const std::string& session) {
    return Sql(session, "SELECT k, v FROM T ORDER BY k");
  }

  absl::Status Insert(const std::string& session, int64_t k,
                      const std::string& v, const std::string& table = "T") {
    spanner_api::CommitRequest request;
    request.set_session(session);
    request.mutable_single_use_transaction()->mutable_read_write();
    auto* insert = request.add_mutations()->mutable_insert();
    insert->set_table(table);
    insert->add_columns("k");
    insert->add_columns("v");
    auto* values = insert->add_values();
    values->add_values()->set_string_value(absl::StrCat(k));
    values->add_values()->set_string_value(v);
    spanner_api::CommitResponse response;
    return Commit(request, &response);
  }

  // Also counts as a use of the session.
  std::shared_ptr<Session> GetSessionObject(const std::string& session) {
    return test_env()->env()->session_manager()->GetSession(session).value();
  }

  int EphemeralCount() {
    return test_env()->env()->session_manager()->ephemeral_session_count();
  }

  // Watches the destruction of an ephemeral session's copy: `destroyed`
  // becomes 1 if the session manager's lock was free then, 2 if it was held.
  struct CopyWatch {
    std::weak_ptr<Database> copy;
    std::shared_ptr<std::atomic<int>> destroyed;
  };
  CopyWatch WatchCopy(const std::string& session) {
    std::shared_ptr<Database> copy = GetSessionObject(session)->database();
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    SessionManager* session_manager = test_env()->env()->session_manager();
    const std::string database_uri = test_database_uri_;
    copy->set_destroy_hook_for_testing([=] {
      // The lock is free if another thread can take it. If this thread holds
      // it, that one finishes after this hook, once it is released.
      auto listed = std::make_shared<absl::Notification>();
      std::thread([=] {
        (void)session_manager->ListSessions(database_uri);
        listed->Notify();
      }).detach();
      *destroyed =
          listed->WaitForNotificationWithTimeout(absl::Seconds(5)) ? 1 : 2;
    });
    return CopyWatch{copy, destroyed};
  }

  // Waits, sending no request, until the watched copy's teardown has finished
  // on the sweeping thread: its destroy hook has recorded its result, and its
  // session has released its place, leaving `ephemeral_count` places taken.
  // The weak pointer expires before either, so it is not enough to wait for.
  bool AwaitTeardown(const CopyWatch& watch, int ephemeral_count,
                     absl::Duration timeout) {
    const absl::Time deadline = absl::Now() + timeout;
    while (absl::Now() < deadline) {
      if (*watch.destroyed != 0 && EphemeralCount() == ephemeral_count) {
        return watch.copy.expired();
      }
      absl::SleepFor(absl::Milliseconds(5));
    }
    return false;
  }
};

// (a), (b): a copy has the rows committed before its session was created, and
// its writes and the base's later ones stay where they were made.
TEST_F(EphemeralSessionTest, CopiesAreSnapshotsIsolatedFromEachOther) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string base, CreateSession({}));
  GOOGLESQL_ASSERT_OK(Insert(base, 1, "one"));
  GOOGLESQL_ASSERT_OK(Insert(base, 2, "two"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string a, CreateSession(kEphemeral));
  GOOGLESQL_ASSERT_OK(Insert(base, 3, "three"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string b, CreateSession(kEphemeral));
  GOOGLESQL_ASSERT_OK(Insert(a, 4, "a"));
  GOOGLESQL_ASSERT_OK(Insert(b, 4, "b"));

  EXPECT_THAT(Rows(base),
              IsOkAndHolds(ElementsAre("1,one", "2,two", "3,three")));
  EXPECT_THAT(Rows(a), IsOkAndHolds(ElementsAre("1,one", "2,two", "4,a")));
  EXPECT_THAT(Rows(b),
              IsOkAndHolds(ElementsAre("1,one", "2,two", "3,three", "4,b")));
  EXPECT_TRUE(GetSessionObject(a)->ephemeral());
  EXPECT_FALSE(GetSessionObject(base)->ephemeral());
  EXPECT_NE(GetSessionObject(a)->database(), GetSessionObject(b)->database());
  EXPECT_EQ(GetSessionObject(a)->base_database(),
            GetSessionObject(base)->database());
  EXPECT_EQ(EphemeralCount(), 2);
}

// (e): a copy answers INFORMATION_SCHEMA and fails statements as its base.
TEST_F(EphemeralSessionTest, SchemaAndErrorsMatchTheBase) {
  GOOGLESQL_ASSERT_OK(UpdateDatabaseDdl(
      test_database_uri_,
      {"CREATE UNIQUE INDEX TByV ON T (v)",
       "CREATE TABLE C (k INT64 NOT NULL, c INT64) PRIMARY KEY (k, c), "
       "INTERLEAVE IN PARENT T ON DELETE CASCADE"}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string base, CreateSession({}));
  GOOGLESQL_ASSERT_OK(Insert(base, 1, "one"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));

  for (const std::string sql : {
           "SELECT table_catalog, table_schema, table_name, column_name, "
           "ordinal_position, spanner_type, is_nullable FROM "
           "INFORMATION_SCHEMA.COLUMNS ORDER BY table_schema, table_name, "
           "ordinal_position",
           "SELECT table_name, index_name, index_type, is_unique FROM "
           "INFORMATION_SCHEMA.INDEXES ORDER BY table_name, index_name",
           "SELECT table_name, parent_table_name, on_delete_action FROM "
           "INFORMATION_SCHEMA.TABLES WHERE table_schema = '' "
           "ORDER BY table_name",
       }) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> base_rows,
                                   Sql(base, sql));
    EXPECT_FALSE(base_rows.empty());
    EXPECT_THAT(Sql(copy, sql), IsOkAndHolds(base_rows)) << sql;
  }
  auto expect_same_error = [&](absl::Status base_error, absl::Status error) {
    EXPECT_FALSE(base_error.ok());
    EXPECT_THAT(error,
                StatusIs(base_error.code(), std::string(base_error.message())));
  };
  expect_same_error(Insert(base, 1, "again"), Insert(copy, 1, "again"));
  expect_same_error(Insert(base, 2, "one"), Insert(copy, 2, "one"));
  expect_same_error(Insert(base, 5, "x", "Missing"),
                    Insert(copy, 5, "x", "Missing"));
  expect_same_error(Sql(base, "SELECT * FROM Missing").status(),
                    Sql(copy, "SELECT * FROM Missing").status());

  // Listed with the base's sessions, labels included.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<spanner_api::Session> sessions,
                                 ListSessions());
  ASSERT_EQ(sessions.size(), 2);
  for (const spanner_api::Session& session : sessions) {
    EXPECT_EQ(session.labels().contains(kEphemeralSessionLabel),
              session.name() == copy);
  }
}

// (f): DDL goes to the base; its copies keep their schema.
TEST_F(EphemeralSessionTest, BaseDdlLeavesCopiesAlone) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string base, CreateSession({}));
  GOOGLESQL_ASSERT_OK(Insert(base, 1, "one"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> ddl,
                                 GetDatabaseDdl());

  GOOGLESQL_ASSERT_OK(UpdateDatabaseDdl(
      test_database_uri_, {"ALTER TABLE T ADD COLUMN w INT64",
                           "CREATE TABLE U (k INT64) PRIMARY KEY (k)"}));

  EXPECT_THAT(Sql(base, "SELECT k, w FROM T"),
              IsOkAndHolds(ElementsAre("1,NULL")));
  EXPECT_THAT(Sql(copy, "SELECT k, w FROM T"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Unrecognized name: w")));
  EXPECT_THAT(Sql(copy, "SELECT * FROM U"),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("Table not found: U")));
  EXPECT_THAT(Rows(copy), IsOkAndHolds(ElementsAre("1,one")));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> new_ddl,
                                 GetDatabaseDdl());
  EXPECT_EQ(new_ddl.size(), ddl.size() + 1);
  // A new copy has the new schema.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string later, CreateSession(kEphemeral));
  EXPECT_THAT(Sql(later, "SELECT k, w FROM T"),
              IsOkAndHolds(ElementsAre("1,NULL")));
}

// (d): sequences and identity columns continue from the base's counters, and
// the copy's counters go with its session.
TEST_F(EphemeralSessionTest, SequencesContinueFromTheBase) {
  GOOGLESQL_ASSERT_OK(DropDatabase());
  GOOGLESQL_ASSERT_OK(CreateDatabase(
      {R"(CREATE SEQUENCE seq OPTIONS (sequence_kind = 'bit_reversed_positive'))",
       R"(CREATE TABLE S (
            id INT64 DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)),
            c INT64,
          ) PRIMARY KEY (id))",
       R"(CREATE TABLE I (
            id INT64 GENERATED BY DEFAULT AS IDENTITY (BIT_REVERSED_POSITIVE),
            c INT64,
          ) PRIMARY KEY (id))"}));
  auto draw = [&](const std::string& session, int c) {
    spanner_api::CommitRequest request;
    request.set_session(session);
    request.mutable_single_use_transaction()->mutable_read_write();
    for (const std::string table : {"S", "I"}) {
      auto* insert = request.add_mutations()->mutable_insert();
      insert->set_table(table);
      insert->add_columns("c");
      insert->add_values()->add_values()->set_string_value(absl::StrCat(c));
    }
    spanner_api::CommitResponse response;
    return Commit(request, &response);
  };
  auto ids = [&](const std::string& session)
      -> absl::StatusOr<std::vector<std::string>> {
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::vector<std::string> ids,
        Sql(session,
            "SELECT CONCAT('s:', CAST(id AS STRING)) FROM S UNION ALL "
            "SELECT CONCAT('i:', CAST(id AS STRING)) FROM I"));
    std::sort(ids.begin(), ids.end());
    return ids;
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string base, CreateSession({}));
  GOOGLESQL_ASSERT_OK(draw(base, 1));
  GOOGLESQL_ASSERT_OK(draw(base, 2));
  const size_t counters = SequenceCounterCount();

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  EXPECT_EQ(SequenceCounterCount(), counters + 2);
  // Restarted counters would draw ids the copied rows already have.
  GOOGLESQL_ASSERT_OK(draw(copy, 3));
  GOOGLESQL_ASSERT_OK(draw(copy, 4));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> copy_ids, ids(copy));
  EXPECT_EQ(
      absl::flat_hash_set<std::string>(copy_ids.begin(), copy_ids.end()).size(),
      8);
  // The base's counters did not move: it draws what the copy drew.
  GOOGLESQL_ASSERT_OK(draw(base, 3));
  GOOGLESQL_ASSERT_OK(draw(base, 4));
  EXPECT_THAT(ids(base), IsOkAndHolds(copy_ids));

  GOOGLESQL_ASSERT_OK(DeleteSession(copy));
  EXPECT_EQ(SequenceCounterCount(), counters);
}

// (g): DeleteSession frees the copy, after releasing the session manager's
// lock.
TEST_F(EphemeralSessionTest, DeleteSessionFreesTheCopy) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  CopyWatch watch = WatchCopy(copy);
  ASSERT_FALSE(watch.copy.expired());

  GOOGLESQL_ASSERT_OK(DeleteSession(copy));

  EXPECT_TRUE(watch.copy.expired());
  EXPECT_EQ(*watch.destroyed, 1);
  EXPECT_EQ(EphemeralCount(), 0);
  EXPECT_THAT(GetSession(copy), StatusIs(absl::StatusCode::kNotFound));
}

// (g): an idle ephemeral session is deleted, and its copy freed outside the
// lock, with no request at all: a test process that dies leaves nothing
// behind.
TEST_F(EphemeralSessionTest, IdleCopiesAreFreedWithoutRequests) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  CopyWatch watch = WatchCopy(copy);
  test_env()->AdvanceClock(absl::Seconds(121));

  // The sweeping thread runs every SessionManager::kSweepInterval.
  ASSERT_TRUE(AwaitTeardown(watch, /*ephemeral_count=*/0, absl::Seconds(20)));
  EXPECT_EQ(*watch.destroyed, 1);
}

// (g): any request on an ephemeral session restarts its idle time.
TEST_F(EphemeralSessionTest, RequestsKeepEphemeralSessionsAlive) {
  test_env()->env()->session_manager()->set_sweep_interval_for_testing(
      absl::Milliseconds(5));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string active, CreateSession(kEphemeral));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string idle, CreateSession(kEphemeral));
  CopyWatch active_watch = WatchCopy(active);
  CopyWatch idle_watch = WatchCopy(idle);

  // Nothing reads from here on: reads would wait for the advanced clock.
  test_env()->AdvanceClock(absl::Seconds(90));
  GOOGLESQL_ASSERT_OK(GetSession(active));
  test_env()->AdvanceClock(absl::Seconds(40));

  ASSERT_TRUE(
      AwaitTeardown(idle_watch, /*ephemeral_count=*/1, absl::Seconds(10)));
  EXPECT_EQ(*idle_watch.destroyed, 1);
  EXPECT_FALSE(active_watch.copy.expired());
  EXPECT_THAT(GetSession(idle), StatusIs(absl::StatusCode::kNotFound));

  test_env()->AdvanceClock(absl::Seconds(121));
  ASSERT_TRUE(
      AwaitTeardown(active_watch, /*ephemeral_count=*/0, absl::Seconds(10)));
  EXPECT_EQ(*active_watch.destroyed, 1);
}

// (g): a lookup of an expired ephemeral session deletes it and frees its copy
// outside the lock, as for ordinary sessions, without waiting for a sweep.
TEST_F(EphemeralSessionTest, LookupOfAnExpiredEphemeralSessionFreesTheCopy) {
  test_env()->env()->session_manager()->set_sweep_interval_for_testing(
      absl::Hours(1));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  CopyWatch watch = WatchCopy(copy);

  config::set_ephemeral_session_idle_timeout(absl::Seconds(5));
  test_env()->AdvanceClock(absl::Seconds(6));
  EXPECT_THAT(GetSession(copy), StatusIs(absl::StatusCode::kNotFound));
  EXPECT_TRUE(watch.copy.expired());
  EXPECT_EQ(*watch.destroyed, 1);
  EXPECT_EQ(EphemeralCount(), 0);
}

// (h): dropping the base deletes its ephemeral sessions and frees their
// copies; a base re-created under the same name does not revive them.
TEST_F(EphemeralSessionTest, DropDatabaseDeletesEphemeralSessions) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string copy, CreateSession(kEphemeral));
  CopyWatch watch = WatchCopy(copy);

  GOOGLESQL_ASSERT_OK(DropDatabase());

  EXPECT_TRUE(watch.copy.expired());
  EXPECT_EQ(*watch.destroyed, 1);
  EXPECT_EQ(EphemeralCount(), 0);
  // As for ordinary sessions: the database is looked up first.
  const absl::Status database_not_found =
      error::DatabaseNotFound(test_database_uri_);
  EXPECT_THAT(Rows(copy), StatusIs(database_not_found.code(),
                                   std::string(database_not_found.message())));

  GOOGLESQL_ASSERT_OK(CreateDatabase(
      {"CREATE TABLE T (k INT64 NOT NULL, v STRING(MAX)) PRIMARY KEY (k)"}));
  const absl::Status session_not_found = error::SessionNotFound(copy);
  EXPECT_THAT(Rows(copy), StatusIs(session_not_found.code(),
                                   std::string(session_not_found.message())));
}

// (h): a drop that runs while a copy is being made wins; the copy is not
// published and nothing of it remains.
TEST_F(EphemeralSessionTest, DropDuringCopyFailsCreateSession) {
  std::shared_ptr<Database> database = test_env()
                                           ->env()
                                           ->database_manager()
                                           ->GetDatabase(test_database_uri_)
                                           .value();
  database->backend()->set_ephemeral_copy_hook_for_testing([&](absl::Time) {
    std::thread dropper([&] { GOOGLESQL_EXPECT_OK(DropDatabase()); });
    dropper.join();
  });

  EXPECT_THAT(CreateSession(kEphemeral), StatusIs(absl::StatusCode::kNotFound));
  database->backend()->set_ephemeral_copy_hook_for_testing(nullptr);
  EXPECT_EQ(EphemeralCount(), 0);
  EXPECT_THAT(test_env()->env()->session_manager()->ListSessions(
                  test_database_uri_, /*include_multiplex_sessions=*/true),
              IsOkAndHolds(IsEmpty()));
}

// (j)
TEST_F(EphemeralSessionTest, MultiplexedEphemeralSessionsAreRefused) {
  EXPECT_THAT(
      CreateSession(kEphemeral, /*multiplexed=*/true),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("multiplexed")));
  EXPECT_EQ(EphemeralCount(), 0);
}

// (k)
TEST_F(EphemeralSessionTest, OpenCopiesAreCapped) {
  config::set_max_ephemeral_sessions(2);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string first, CreateSession(kEphemeral));
  GOOGLESQL_ASSERT_OK(CreateSession(kEphemeral).status());
  EXPECT_THAT(CreateSession(kEphemeral),
              StatusIs(absl::StatusCode::kResourceExhausted));
  // Ordinary sessions are not capped.
  GOOGLESQL_ASSERT_OK(CreateSession({}).status());
  GOOGLESQL_ASSERT_OK(DeleteSession(first));
  GOOGLESQL_ASSERT_OK(CreateSession(kEphemeral).status());
  EXPECT_EQ(EphemeralCount(), 2);

  // A batch that does not fit is refused whole.
  config::set_max_ephemeral_sessions(4);
  EXPECT_THAT(BatchCreateSessions(kEphemeral, 3),
              StatusIs(absl::StatusCode::kResourceExhausted));
  EXPECT_EQ(EphemeralCount(), 2);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> batch,
                                 BatchCreateSessions(kEphemeral, 2));
  EXPECT_EQ(batch.size(), 2);
  EXPECT_EQ(EphemeralCount(), 4);
  EXPECT_THAT(ListSessions(), IsOkAndHolds(testing::SizeIs(5)));
}

// (k): a copy counts until it is destroyed. A request in flight keeps a
// deleted session, and its copy, alive; its place is not free before then.
TEST_F(EphemeralSessionTest, CopiesCountUntilTheyAreDestroyed) {
  config::set_max_ephemeral_sessions(1);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string first, CreateSession(kEphemeral));
  // What a streaming query on the session holds while it runs.
  std::shared_ptr<Session> in_flight = GetSessionObject(first);
  std::weak_ptr<Database> copy = in_flight->database();

  GOOGLESQL_ASSERT_OK(DeleteSession(first));
  EXPECT_FALSE(copy.expired());
  EXPECT_THAT(CreateSession(kEphemeral),
              StatusIs(absl::StatusCode::kResourceExhausted));

  in_flight.reset();
  EXPECT_TRUE(copy.expired());
  GOOGLESQL_EXPECT_OK(CreateSession(kEphemeral).status());
}

// (l): only the value "true" makes a session ephemeral.
TEST_F(EphemeralSessionTest, OtherLabelValuesMakeOrdinarySessions) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string writer, CreateSession({}));
  std::vector<Labels> ordinary = {
      {},
      {{"other", "true"}},
      {{kEphemeralSessionLabel, "false"}},
      {{kEphemeralSessionLabel, "yes"}},
      {{kEphemeralSessionLabel, "1"}},
      {{kEphemeralSessionLabel, ""}},
  };
  std::vector<std::string> sessions;
  for (const Labels& labels : ordinary) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string session, CreateSession(labels));
    EXPECT_FALSE(GetSessionObject(session)->ephemeral());
    EXPECT_EQ(GetSessionObject(session)->database(),
              GetSessionObject(writer)->database());
    sessions.push_back(session);
  }
  GOOGLESQL_ASSERT_OK(Insert(writer, 1, "one"));
  for (const std::string& session : sessions) {
    EXPECT_THAT(Rows(session), IsOkAndHolds(ElementsAre("1,one")));
  }
  EXPECT_EQ(EphemeralCount(), 0);
}

// A base whose schema cannot be copied fails CreateSession; ordinary sessions
// still work.
TEST_F(EphemeralSessionTest, UncopyableSchemaFailsCreateSession) {
  test::ScopedEmulatorFeatureFlagsSetter flags(EmulatorFeatureFlags::Flags{
      .enable_user_defined_functions = true,
  });
  GOOGLESQL_ASSERT_OK(DropDatabase());
  GOOGLESQL_ASSERT_OK(
      CreateDatabase({"CREATE FUNCTION f(x INT64 DEFAULT 1) "
                      "RETURNS INT64 SQL SECURITY INVOKER "
                      "AS (x + 1)"}));
  EXPECT_THAT(CreateSession(kEphemeral),
              StatusIs(absl::StatusCode::kUnimplemented,
                       HasSubstr("Cannot create an ephemeral session")));
  EXPECT_EQ(EphemeralCount(), 0);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::string session, CreateSession({}));
  EXPECT_THAT(Sql(session, "SELECT f(1)"), IsOkAndHolds(ElementsAre("2")));
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
