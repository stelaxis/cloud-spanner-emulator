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

// A schema someone still uses must survive its removal from the versioned
// catalog. Each test holds a schema while newer schemas are published and the
// catalog garbage-collects the old one, then uses it. A controlled clock makes
// the collection deterministic: schema changes two hours apart expire every
// schema older than the one-hour retention period. Run under AddressSanitizer
// to catch a use of a freed schema.

#include <atomic>
#include <cstdint>
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
#include "backend/access/read.h"
#include "backend/database/database.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/function_catalog.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using ::googlesql_base::testing::StatusIs;

class SchemaLifetimeTest : public testing::Test {
 protected:
  SchemaLifetimeTest()
      // Behind the wall clock, so that reads never wait for their timestamp.
      : now_micros_(absl::ToUnixMicros(absl::Now() - absl::Hours(48))),
        clock_([this]() { return absl::FromUnixMicros(now_micros_.load()); }) {}

  void CreateDatabase() {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        db_, Database::Create(
                 &clock_, "test-db",
                 SchemaChangeOperation{.statements = {
                                           R"(CREATE SEQUENCE seq OPTIONS (
                                  sequence_kind = 'bit_reversed_positive'))",
                                           R"(CREATE TABLE T (
                                  k INT64 NOT NULL,
                                  v INT64,
                                ) PRIMARY KEY (k))"}}));
    // The creation schema is never collected; publish a collectable one.
    PublishSchema();
  }

  // Moves the clock two hours on, past the retention period, and publishes a
  // new schema. The schema change collects every schema older than the
  // retention period except the one in effect at its start.
  void PublishSchema() {
    now_micros_ += absl::ToInt64Microseconds(absl::Hours(2));
    int completed;
    absl::Time commit_ts;
    absl::Status backfill;
    GOOGLESQL_ASSERT_OK(db_->UpdateSchema(
        SchemaChangeOperation{
            .statements = {absl::StrCat("CREATE TABLE X", ++tables_,
                                        " (k INT64 NOT NULL) PRIMARY KEY (k)")}},
        &completed, &commit_ts, &backfill));
    GOOGLESQL_ASSERT_OK(backfill);
  }

  std::unique_ptr<ReadWriteTransaction> BeginReadWrite() {
    return db_->CreateReadWriteTransaction(ReadWriteOptions(), RetryState())
        .value();
  }

  std::atomic<int64_t> now_micros_;
  Clock clock_;
  std::unique_ptr<Database> db_;
  int tables_ = 0;
};

// Sequence functions evaluate against the latest schema. A schema change
// publishes a newer one after the evaluator loaded it, and another collects
// it, while the evaluator is still using it.
TEST_F(SchemaLifetimeTest, SequenceFunctionKeepsTheSchemaItLoaded) {
  CreateDatabase();
  auto txn = BeginReadWrite();
  const Schema* analysis_schema = txn->schema();
  PublishSchema();  // The evaluator will load this one.

  absl::Notification loaded, resume;
  std::atomic<int> hook_calls = 0;
  db_->query_engine()->mutable_function_catalog()->
      set_schema_loaded_hook_for_testing([&]() {
        if (hook_calls++ == 0) {
          loaded.Notify();
          resume.WaitForNotification();
        }
      });

  absl::StatusOr<QueryResult> result;
  std::thread query([&]() {
    result = db_->query_engine()->ExecuteSql(
        Query{.sql = "SELECT GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)"},
        QueryContext{.schema = analysis_schema,
                     .reader = txn.get(),
                     .writer = txn.get(),
                     .commit_timestamp_tracker =
                         txn->commit_timestamp_tracker(),
                     .allow_read_write_only_functions = true,
                     .is_read_only_txn = false});
  });
  loaded.WaitForNotification();
  PublishSchema();
  PublishSchema();  // Collects the schema the evaluator loaded.
  resume.Notify();
  query.join();

  GOOGLESQL_ASSERT_OK(result);
  ASSERT_TRUE(result->rows->Next());
  EXPECT_GT(result->rows->ColumnValue(0).int64_value(), 0);
}

// A read-write transaction hands out the latest schema before its first data
// operation. It keeps that schema alive, and its first data operation aborts
// because a schema change replaced it.
TEST_F(SchemaLifetimeTest, ReadWriteTransactionKeepsTheSchemaItHandedOut) {
  CreateDatabase();
  auto txn = BeginReadWrite();
  PublishSchema();
  const Schema* schema = txn->schema();
  PublishSchema();
  PublishSchema();  // Collects `schema`.

  const Table* table = schema->FindTable("T");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->Name(), "T");

  std::unique_ptr<RowCursor> cursor;
  EXPECT_THAT(
      txn->Read(ReadArg{.table = "T", .key_set = KeySet::All(), .columns = {"k"}},
                &cursor),
      StatusIs(absl::StatusCode::kAborted));

  // Without a schema handed out, the first data operation takes the latest.
  auto fresh = BeginReadWrite();
  PublishSchema();
  GOOGLESQL_EXPECT_OK(fresh->Read(
      ReadArg{.table = "T", .key_set = KeySet::All(), .columns = {"k"}},
      &cursor));
}

// A read-only transaction's cursor keeps the schema its columns belong to,
// after the transaction is gone and the schema has been collected.
TEST_F(SchemaLifetimeTest, ReadOnlyCursorKeepsItsSchema) {
  CreateDatabase();
  {
    auto txn = BeginReadWrite();
    Mutation m;
    m.AddWriteOp(MutationOpType::kInsert, "T", {"k", "v"},
                 {{googlesql::values::Int64(1), googlesql::values::Int64(10)}});
    GOOGLESQL_ASSERT_OK(txn->Write(m));
    GOOGLESQL_ASSERT_OK(txn->Commit());
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<ReadOnlyTransaction> ro,
      db_->CreateReadOnlyTransaction(ReadOnlyOptions()));
  std::unique_ptr<RowCursor> cursor;
  GOOGLESQL_ASSERT_OK(ro->Read(
      ReadArg{.table = "T", .key_set = KeySet::All(), .columns = {"k", "v"}},
      &cursor));
  ro.reset();
  PublishSchema();
  PublishSchema();  // Collects the schema the cursor's columns belong to.

  ASSERT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->ColumnName(0), "k");
  EXPECT_EQ(cursor->ColumnName(1), "v");
  EXPECT_TRUE(cursor->ColumnType(1)->IsInt64());
  EXPECT_EQ(cursor->ColumnValue(1).int64_value(), 10);
  EXPECT_FALSE(cursor->Next());
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
