//
// Copyright 2026 The Stelaxis Authors
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

// Read-write transactions, queries and schema changes of one database running
// on concurrent threads. Run under ThreadSanitizer to catch data races.

#include <atomic>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/value.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/write.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/database.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;

constexpr char kDatabaseId[] = "test-db";

class DatabaseConcurrencyTest : public testing::Test {
 protected:
  std::unique_ptr<ReadWriteTransaction> Begin(Database* db) {
    return db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState())
        .value();
  }

  absl::Status UpdateSchema(Database* db, const std::string& statement) {
    int completed_statements;
    absl::Time commit_ts;
    absl::Status backfill_status;
    absl::Status status =
        db->UpdateSchema(SchemaChangeOperation{.statements = {statement}},
                         &completed_statements, &commit_ts, &backfill_status);
    return status.ok() ? backfill_status : status;
  }

  Clock clock_;
};

// A table without effectors or verifiers: the first writes to it look up the
// shared action registry concurrently, which must not insert into it.
TEST_F(DatabaseConcurrencyTest, SimultaneousFirstWritesToUnseededTables) {
  constexpr int kTables = 32;
  constexpr int kThreads = 16;
  std::vector<std::string> ddl;
  for (int t = 0; t < kTables; ++t) {
    ddl.push_back(absl::StrCat("CREATE TABLE T", t,
                               " (k INT64 NOT NULL, v INT64) PRIMARY KEY (k)"));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> db,
      Database::Create(&clock_, kDatabaseId,
                       SchemaChangeOperation{.statements = ddl}));

  absl::Notification start;
  std::atomic<int> failures = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      start.WaitForNotification();
      for (int n = 0; n < kTables; ++n) {
        int t = (n + i) % kTables;
        auto txn = Begin(db.get());
        Mutation m;
        m.AddWriteOp(MutationOpType::kInsert, absl::StrCat("T", t),
                     {"k", "v"}, {{Int64(i), Int64(i)}});
        absl::Status status = txn->Write(m);
        if (status.ok()) status = txn->Commit();
        if (!status.ok()) ++failures;
      }
    });
  }
  start.Notify();
  for (std::thread& thread : threads) thread.join();
  // Every writer inserts its own key, so nothing conflicts.
  EXPECT_EQ(failures, 0);
}

// Schema changes replace the action registry and the function catalog's
// latest schema while other threads are in the middle of writes and DML that
// use them: a transaction keeps the schema and registry it started with.
TEST_F(DatabaseConcurrencyTest, SchemaChangesRaceInFlightWritesAndQueries) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> db,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{.statements = {
                                    R"(CREATE SEQUENCE seq OPTIONS (
                                         sequence_kind = 'bit_reversed_positive'))",
                                    R"(CREATE TABLE W (
                                         k INT64 NOT NULL,
                                         v INT64 DEFAULT
                                           (GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)),
                                         c INT64,
                                       ) PRIMARY KEY (k))",
                                    "CREATE INDEX WByC ON W (c)"}}));

  constexpr int kWriters = 4;
  constexpr int kSchemaChanges = 30;
  std::atomic<bool> stop = false;
  std::atomic<int> committed = 0;
  std::atomic<int> unexpected = 0;
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w]() {
      for (int64_t i = 0; !stop; ++i) {
        auto txn = Begin(db.get());
        absl::Status status;
        if (i % 2 == 0) {
          Mutation m;
          m.AddWriteOp(MutationOpType::kInsert, "W", {"k", "c"},
                       {{Int64(w * 1000000 + i), Int64(i)}});
          status = txn->Write(m);
        } else {
          status =
              db->query_engine()
                  ->ExecuteSql(
                      Query{.sql = "INSERT INTO W (k, c) VALUES (@k, @c)",
                            .declared_params = {{"k", Int64(w * 1000000 + i)},
                                                {"c", Int64(i)}}},
                      QueryContext{.schema = txn->schema(),
                                   .reader = txn.get(),
                                   .writer = txn.get(),
                                   .commit_timestamp_tracker =
                                       txn->commit_timestamp_tracker(),
                                   .allow_read_write_only_functions = true,
                                   .is_read_only_txn = false})
                  .status();
        }
        if (status.ok()) status = txn->Commit();
        if (status.ok()) {
          ++committed;
        } else if (status.code() != absl::StatusCode::kAborted) {
          ++unexpected;
          ADD_FAILURE() << status;
        }
      }
    });
  }
  // Each schema change starts once writers are committing, so that it lands in
  // the middle of their operations.
  auto wait_for_commits_beyond = [&](int n) {
    absl::Time deadline = absl::Now() + absl::Seconds(30);
    while (committed <= n && absl::Now() < deadline) {
      absl::SleepFor(absl::Microseconds(100));
    }
    return committed > n;
  };
  for (int i = 0; i < kSchemaChanges; ++i) {
    ASSERT_TRUE(wait_for_commits_beyond(committed));
    GOOGLESQL_EXPECT_OK(UpdateSchema(
        db.get(),
        absl::StrCat("CREATE TABLE X", i, " (k INT64 NOT NULL) PRIMARY KEY (k)")));
  }
  // Writers keep committing on the final schema.
  EXPECT_TRUE(wait_for_commits_beyond(committed));
  stop = true;
  for (std::thread& thread : writers) thread.join();
  EXPECT_EQ(unexpected, 0);
}

// Two schema changes reconcile change stream churners in commit order. The
// first pauses after reading its new schema; the second must not be able to
// commit a newer schema and reconcile before it.
TEST_F(DatabaseConcurrencyTest, SchemaChangesReconcileChurnersInOrder) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> db,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{.statements = {
                                    "CREATE TABLE T (k INT64) PRIMARY KEY (k)"}}));
  ChangeStreamPartitionChurner* churner =
      db->get_change_stream_partition_churner();
  ASSERT_EQ(churner->GetNumThreads(), 0);

  std::atomic<int> hook_calls = 0;
  absl::Notification first_paused, second_done;
  db->set_before_churner_update_hook_for_testing([&]() {
    if (hook_calls++ == 0) {
      first_paused.Notify();
      // Without serialization the second schema change completes here.
      second_done.WaitForNotificationWithTimeout(absl::Milliseconds(300));
    }
  });

  std::thread first([&]() {
    GOOGLESQL_EXPECT_OK(UpdateSchema(db.get(), "CREATE CHANGE STREAM s1 FOR ALL"));
  });
  first_paused.WaitForNotification();
  std::thread second([&]() {
    GOOGLESQL_EXPECT_OK(UpdateSchema(db.get(), "CREATE CHANGE STREAM s2 FOR ALL"));
    second_done.Notify();
  });
  first.join();
  second.join();
  EXPECT_EQ(churner->GetNumThreads(), 2);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
