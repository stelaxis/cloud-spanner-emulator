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

// Database::CreateEphemeralCopy: what a copy holds, and that it is taken at
// one timestamp while other threads commit.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/database/database.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/feature_flags.h"
#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/value.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/scoped_feature_flags_setter.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql_base::testing::IsOkAndHolds;
using googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr char kDatabaseId[] = "test-db";

size_t SequenceCounterCount() {
  absl::MutexLock lock(Sequence::SequenceMutex);
  return Sequence::SequenceLastValues.size();
}

// Pauses the first call of a hook until Resume. Later calls pass through.
class PausePoint {
 public:
  void MaybePause(absl::Time arg) {
    if (paused_.exchange(true)) return;
    arg_ = arg;
    reached_.Notify();
    resume_.WaitForNotification();
  }
  absl::Time WaitUntilReached() {
    reached_.WaitForNotification();
    return arg_;
  }
  void Resume() { resume_.Notify(); }

 private:
  std::atomic<bool> paused_ = false;
  absl::Time arg_;
  absl::Notification reached_;
  absl::Notification resume_;
};

class EphemeralCopyTest : public testing::Test {
 protected:
  absl::StatusOr<std::unique_ptr<Database>> Create(
      std::vector<std::string> statements) {
    return Database::Create(&clock_, kDatabaseId,
                            SchemaChangeOperation{.statements = statements});
  }

  std::unique_ptr<Database> CreateKv() {
    return Create({"CREATE TABLE T (k INT64 NOT NULL, v INT64) PRIMARY KEY (k)",
                   "CREATE INDEX TByV ON T (v)"})
        .value();
  }

  std::unique_ptr<ReadWriteTransaction> Begin(Database* db) {
    return db->CreateReadWriteTransaction(ReadWriteOptions(), RetryState())
        .value();
  }

  // Commits `mutation` in its own transaction.
  absl::Status Apply(Database* db, const Mutation& mutation) {
    std::unique_ptr<ReadWriteTransaction> txn = Begin(db);
    GOOGLESQL_RETURN_IF_ERROR(txn->Write(mutation));
    return txn->Commit();
  }

  absl::Status Insert(Database* db, const std::string& table,
                      std::vector<std::pair<int64_t, int64_t>> rows) {
    Mutation m;
    for (const auto& [k, v] : rows) {
      m.AddWriteOp(MutationOpType::kInsert, table, {"k", "v"},
                   {{Int64(k), Int64(v)}});
    }
    return Apply(db, m);
  }

  // Runs `sql` in a read-only transaction; one string per row.
  absl::StatusOr<std::vector<std::string>> Sql(
      Database* db, const std::string& sql,
      ReadOnlyOptions options = ReadOnlyOptions()) {
    GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ReadOnlyTransaction> txn,
                               db->CreateReadOnlyTransaction(options));
    GOOGLESQL_ASSIGN_OR_RETURN(
        QueryResult result,
        db->query_engine()->ExecuteSql(
            Query{.sql = sql},
            QueryContext{.schema = txn->schema(), .reader = txn.get()}));
    std::vector<std::string> rows;
    while (result.rows->Next()) {
      std::vector<std::string> values;
      for (int i = 0; i < result.rows->NumColumns(); ++i) {
        values.push_back(result.rows->ColumnValue(i).DebugString());
      }
      rows.push_back(absl::StrJoin(values, ","));
    }
    GOOGLESQL_RETURN_IF_ERROR(result.rows->Status());
    return rows;
  }

  absl::StatusOr<std::vector<std::string>> Rows(
      Database* db, ReadOnlyOptions options = ReadOnlyOptions()) {
    return Sql(db, "SELECT k, v FROM T ORDER BY k", options);
  }

  absl::Status UpdateSchema(Database* db, const std::string& statement) {
    int completed_statements;
    absl::Time commit_ts;
    absl::Status backfill_status;
    absl::Status status = db->UpdateSchema(
        SchemaChangeOperation{.statements = {statement},
                              .database_dialect = db->dialect()},
        &completed_statements, &commit_ts, &backfill_status);
    return status.ok() ? backfill_status : status;
  }

  // Makes a copy and returns its timestamp.
  absl::StatusOr<std::unique_ptr<Database>> Copy(
      Database* db, absl::Time* timestamp = nullptr) {
    db->set_ephemeral_copy_hook_for_testing([timestamp](absl::Time t) {
      if (timestamp != nullptr) *timestamp = t;
    });
    absl::StatusOr<std::unique_ptr<Database>> copy = db->CreateEphemeralCopy();
    db->set_ephemeral_copy_hook_for_testing(nullptr);
    return copy;
  }

  Clock clock_;
};

TEST_F(EphemeralCopyTest, CopySeesCommittedRowsAsOneVersionAtItsTimestamp) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}, {2, 20}}));
  Mutation update;
  update.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
                    {{Int64(2), Int64(21)}});
  GOOGLESQL_ASSERT_OK(Apply(base.get(), update));
  Mutation remove;
  remove.AddDeleteOp("T", KeySet(Key({Int64(1)})));
  GOOGLESQL_ASSERT_OK(Apply(base.get(), remove));
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{3, 30}}));

  absl::Time t;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get(), &t));
  EXPECT_THAT(Rows(copy.get()), IsOkAndHolds(ElementsAre("2,21", "3,30")));
  // The index's data is copied too.
  EXPECT_THAT(
      Sql(copy.get(), "SELECT k FROM T@{FORCE_INDEX=TByV} WHERE v > 25"),
      IsOkAndHolds(ElementsAre("3")));
  // Reads at and after the copy's timestamp see the copied rows; before it,
  // none, even where the base had some.
  ReadOnlyOptions at{.bound = TimestampBound::kExactTimestamp, .timestamp = t};
  EXPECT_THAT(Rows(copy.get(), at), IsOkAndHolds(ElementsAre("2,21", "3,30")));
  ReadOnlyOptions before{.bound = TimestampBound::kExactTimestamp,
                         .timestamp = t - absl::Microseconds(1)};
  EXPECT_THAT(Rows(copy.get(), before), IsOkAndHolds(IsEmpty()));
  EXPECT_THAT(Rows(base.get(), before),
              IsOkAndHolds(ElementsAre("2,21", "3,30")));
  // Bounded staleness reads no earlier than the last commit, which is the
  // copy's timestamp.
  ReadOnlyOptions stale{.bound = TimestampBound::kMaxStaleness,
                        .staleness = absl::Hours(1)};
  EXPECT_THAT(Rows(copy.get(), stale),
              IsOkAndHolds(ElementsAre("2,21", "3,30")));
}

TEST_F(EphemeralCopyTest, CopiesAreIsolatedFromTheBaseAndEachOther) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> a, Copy(base.get()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> b, Copy(base.get()));

  GOOGLESQL_ASSERT_OK(Insert(a.get(), "T", {{2, 20}}));
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{3, 30}}));
  GOOGLESQL_ASSERT_OK(Insert(b.get(), "T", {{2, 22}}));

  EXPECT_THAT(Rows(base.get()), IsOkAndHolds(ElementsAre("1,10", "3,30")));
  EXPECT_THAT(Rows(a.get()), IsOkAndHolds(ElementsAre("1,10", "2,20")));
  EXPECT_THAT(Rows(b.get()), IsOkAndHolds(ElementsAre("1,10", "2,22")));

  // Destroying a copy leaves the base and the other copy alone.
  a.reset();
  EXPECT_THAT(Rows(base.get()), IsOkAndHolds(ElementsAre("1,10", "3,30")));
  EXPECT_THAT(Rows(b.get()), IsOkAndHolds(ElementsAre("1,10", "2,22")));
}

// A commit that reserved its timestamp before the copy's is waited for, so
// the copy has all of its rows even though it finishes flushing only after
// the copy picked its timestamp.
TEST_F(EphemeralCopyTest, CopyWaitsForACommitPendingAtItsTimestamp) {
  std::unique_ptr<Database> base = CreateKv();
  PausePoint commit_pause;
  base->set_before_commit_flush_hook_for_testing(
      [&](absl::Time t) { commit_pause.MaybePause(t); });
  std::thread writer([&] {
    GOOGLESQL_EXPECT_OK(Insert(base.get(), "T", {{1, 10}, {2, 20}}));
  });
  const absl::Time commit_timestamp = commit_pause.WaitUntilReached();

  absl::Notification picked;
  absl::Time copy_timestamp;
  base->set_ephemeral_copy_hook_for_testing([&](absl::Time t) {
    copy_timestamp = t;
    picked.Notify();
  });
  // Resumes the commit well after the copy picked its timestamp: a copy that
  // did not wait would be done by then, without the rows.
  std::thread resumer([&] {
    picked.WaitForNotification();
    absl::SleepFor(absl::Milliseconds(200));
    commit_pause.Resume();
  });
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 base->CreateEphemeralCopy());
  resumer.join();
  writer.join();

  EXPECT_LT(commit_timestamp, copy_timestamp);
  EXPECT_THAT(Rows(copy.get()), IsOkAndHolds(ElementsAre("1,10", "2,20")));
}

// A commit that reserves its timestamp after the copy's is left out whole,
// even if it has flushed before the copy reads the rows.
TEST_F(EphemeralCopyTest, CopyLeavesOutACommitAfterItsTimestamp) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}}));
  absl::Time copy_timestamp;
  std::optional<absl::Time> commit_timestamp;
  base->set_ephemeral_copy_hook_for_testing([&](absl::Time t) {
    copy_timestamp = t;
    std::thread writer([&] {
      std::unique_ptr<ReadWriteTransaction> txn = Begin(base.get());
      Mutation m;
      m.AddWriteOp(MutationOpType::kInsert, "T", {"k", "v"},
                   {{Int64(2), Int64(20)}, {Int64(3), Int64(30)}});
      m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
                   {{Int64(1), Int64(11)}});
      GOOGLESQL_EXPECT_OK(txn->Write(m));
      GOOGLESQL_EXPECT_OK(txn->Commit());
      commit_timestamp = txn->GetCommitTimestamp().value();
    });
    writer.join();
  });
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 base->CreateEphemeralCopy());
  ASSERT_TRUE(commit_timestamp.has_value());

  EXPECT_GT(*commit_timestamp, copy_timestamp);
  EXPECT_THAT(Rows(base.get()),
              IsOkAndHolds(ElementsAre("1,11", "2,20", "3,30")));
  EXPECT_THAT(Rows(copy.get()), IsOkAndHolds(ElementsAre("1,10")));
}

// Every copy taken while pairs of rows are committed holds whole pairs.
TEST_F(EphemeralCopyTest, CopiesTakenDuringCommitsHoldWholeCommits) {
  std::unique_ptr<Database> base = CreateKv();
  std::atomic<bool> stop = false;
  std::thread writer([&] {
    for (int64_t i = 0; !stop && i < 1000; ++i) {
      GOOGLESQL_EXPECT_OK(
          Insert(base.get(), "T", {{2 * i, i}, {2 * i + 1, i}}));
    }
  });
  for (int n = 0; n < 50; ++n) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                   Copy(base.get()));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        std::vector<std::string> pairs,
        Sql(copy.get(),
            "SELECT v, COUNT(*) FROM T GROUP BY v HAVING COUNT(*) != 2"));
    EXPECT_THAT(pairs, IsEmpty());
  }
  stop = true;
  writer.join();
}

// Copies of one database are taken on several threads at once, while pairs of
// rows are committed and schema changes run.
TEST_F(EphemeralCopyTest, ConcurrentCopiesDuringCommitsAndSchemaChanges) {
  std::unique_ptr<Database> base = CreateKv();
  std::atomic<bool> stop = false;
  std::thread writer([&] {
    for (int64_t i = 0; !stop && i < 1000; ++i) {
      // A schema change aborts the commits it overlaps.
      absl::Status status =
          Insert(base.get(), "T", {{2 * i, i}, {2 * i + 1, i}});
      if (status.code() != absl::StatusCode::kAborted) {
        GOOGLESQL_EXPECT_OK(status);
      }
    }
  });
  // Paced, so that copies, which wait for schema changes, get their turn.
  std::thread ddl([&] {
    for (int i = 0; i < 20 && !stop; ++i) {
      GOOGLESQL_EXPECT_OK(UpdateSchema(
          base.get(),
          absl::StrCat("CREATE TABLE X", i, " (k INT64) PRIMARY KEY (k)")));
      absl::SleepFor(absl::Milliseconds(2));
    }
  });
  std::vector<std::thread> copiers;
  for (int c = 0; c < 8; ++c) {
    copiers.emplace_back([&] {
      for (int n = 0; n < 10; ++n) {
        absl::StatusOr<std::unique_ptr<Database>> copy =
            base->CreateEphemeralCopy();
        GOOGLESQL_ASSERT_OK(copy);
        EXPECT_THAT(
            Sql(copy->get(), "SELECT v FROM T GROUP BY v HAVING COUNT(*) != 2"),
            IsOkAndHolds(IsEmpty()));
        GOOGLESQL_EXPECT_OK(Insert(copy->get(), "T", {{-1, -1}}));
      }
    });
  }
  for (std::thread& copier : copiers) copier.join();
  stop = true;
  writer.join();
  ddl.join();
}

// Read-write transactions on a copy conflict and commit as they do on the
// database it copies.
TEST_F(EphemeralCopyTest, ReadWriteTransactionsOnACopyBehaveAsOnItsBase) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}, {2, 20}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));

  // Two transactions read and then update keys `k1` and `k2`, interleaved;
  // returns each one's commit status code.
  auto race = [&](Database* db, int64_t k1, int64_t k2) {
    std::unique_ptr<ReadWriteTransaction> t1 = Begin(db);
    std::unique_ptr<ReadWriteTransaction> t2 = Begin(db);
    auto read_and_update = [](ReadWriteTransaction* txn,
                              int64_t k) -> absl::Status {
      std::unique_ptr<RowCursor> cursor;
      GOOGLESQL_RETURN_IF_ERROR(
          txn->Read(ReadArg{.table = "T",
                            .key_set = KeySet(Key({Int64(k)})),
                            .columns = {"v"}},
                    &cursor));
      while (cursor->Next()) {
      }
      Mutation m;
      m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
                   {{Int64(k), Int64(k * 100)}});
      return txn->Write(m);
    };
    absl::Status s1 = read_and_update(t1.get(), k1);
    absl::Status s2 = read_and_update(t2.get(), k2);
    if (s1.ok()) s1 = t1->Commit();
    if (s2.ok()) s2 = t2->Commit();
    return std::vector<absl::StatusCode>{s1.code(), s2.code()};
  };
  const std::vector<absl::StatusCode> conflicting = {
      absl::StatusCode::kOk, absl::StatusCode::kAborted};
  const std::vector<absl::StatusCode> disjoint = {absl::StatusCode::kOk,
                                                  absl::StatusCode::kOk};
  EXPECT_EQ(race(base.get(), 1, 1), conflicting);
  EXPECT_EQ(race(copy.get(), 1, 1), conflicting);
  EXPECT_EQ(race(base.get(), 1, 2), disjoint);
  EXPECT_EQ(race(copy.get(), 1, 2), disjoint);
}

// A copy's sequences and identity columns continue from the base's counters,
// then draw independently of it; destroying the copy releases its counters.
TEST_F(EphemeralCopyTest, SequencesContinueFromTheBaseCounters) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> base,
                                 Create({R"(CREATE SEQUENCE seq OPTIONS (
                   sequence_kind = 'bit_reversed_positive'))",
                                         R"(CREATE TABLE S (
                   id INT64 DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)),
                   c INT64,
                 ) PRIMARY KEY (id))",
                                         R"(CREATE TABLE I (
                   id INT64 GENERATED BY DEFAULT AS IDENTITY
                     (BIT_REVERSED_POSITIVE),
                   c INT64,
                 ) PRIMARY KEY (id))"}));
  auto draw = [&](Database* db, int64_t c) {
    Mutation m;
    m.AddWriteOp(MutationOpType::kInsert, "S", {"c"}, {{Int64(c)}});
    m.AddWriteOp(MutationOpType::kInsert, "I", {"c"}, {{Int64(c)}});
    return Apply(db, m);
  };
  auto ids = [&](Database* db, int64_t c) {
    return Sql(db, absl::StrCat("SELECT s.id, i.id FROM S s, I i WHERE s.c = ",
                                c, " AND i.c = ", c));
  };
  GOOGLESQL_ASSERT_OK(draw(base.get(), 1));
  GOOGLESQL_ASSERT_OK(draw(base.get(), 2));
  const size_t base_counters = SequenceCounterCount();

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));
  EXPECT_EQ(SequenceCounterCount(), base_counters + 2);
  // A counter restarted in the copy would draw the base's first ids again,
  // which the copied rows hold.
  GOOGLESQL_ASSERT_OK(draw(copy.get(), 3));
  GOOGLESQL_ASSERT_OK(draw(base.get(), 3));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> copy_ids,
                                 ids(copy.get(), 3));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::string> base_ids,
                                 ids(base.get(), 3));
  ASSERT_EQ(copy_ids.size(), 1);
  // Both continue from the same counters, so they draw the same values.
  EXPECT_EQ(copy_ids, base_ids);
  EXPECT_THAT(Sql(copy.get(), "SELECT COUNT(DISTINCT id) FROM S"),
              IsOkAndHolds(ElementsAre("3")));

  copy.reset();
  EXPECT_EQ(SequenceCounterCount(), base_counters);
  GOOGLESQL_ASSERT_OK(draw(base.get(), 4));
  EXPECT_THAT(Sql(base.get(), "SELECT COUNT(DISTINCT id) FROM I"),
              IsOkAndHolds(ElementsAre("4")));
}

// Schema changes of the base after the copy do not reach it, and a copy's own
// schema changes do not reach the base.
TEST_F(EphemeralCopyTest, SchemaChangesStayInTheirDatabase) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));

  GOOGLESQL_ASSERT_OK(
      UpdateSchema(base.get(), "ALTER TABLE T ADD COLUMN w INT64"));
  GOOGLESQL_ASSERT_OK(
      UpdateSchema(base.get(), "CREATE TABLE U (k INT64) PRIMARY KEY (k)"));
  GOOGLESQL_ASSERT_OK(UpdateSchema(base.get(), "DROP INDEX TByV"));

  EXPECT_NE(base->GetLatestSchema()->FindTable("U"), nullptr);
  EXPECT_EQ(copy->GetLatestSchema()->FindTable("U"), nullptr);
  EXPECT_EQ(copy->GetLatestSchema()->FindTable("T")->FindColumn("w"), nullptr);
  EXPECT_THAT(Sql(copy.get(), "SELECT k FROM T@{FORCE_INDEX=TByV}"),
              IsOkAndHolds(ElementsAre("1")));
  EXPECT_THAT(Sql(copy.get(), "SELECT w FROM T"),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // A copy of the changed base has the change.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> later,
                                 Copy(base.get()));
  EXPECT_THAT(Sql(later.get(), "SELECT k, w FROM T"),
              IsOkAndHolds(ElementsAre("1,NULL")));
}

// Table and column IDs are the object's name and a counter. A copy's counters
// continue from the base's, so a table or column dropped and created again in
// the copy does not get its old ID back, with the copied data under it.
TEST_F(EphemeralCopyTest, SchemaChangesOnACopyAssignNewIds) {
  std::unique_ptr<Database> base = CreateKv();
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "T", {{1, 10}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));

  GOOGLESQL_ASSERT_OK(UpdateSchema(copy.get(), "DROP INDEX TByV"));
  // Each time with the next column ID.
  for (int i = 0; i < 8; ++i) {
    GOOGLESQL_ASSERT_OK(
        UpdateSchema(copy.get(), "ALTER TABLE T DROP COLUMN v"));
    GOOGLESQL_ASSERT_OK(
        UpdateSchema(copy.get(), "ALTER TABLE T ADD COLUMN v INT64"));
    EXPECT_THAT(Rows(copy.get()), IsOkAndHolds(ElementsAre("1,NULL"))) << i;
  }

  GOOGLESQL_ASSERT_OK(UpdateSchema(copy.get(), "DROP TABLE T"));
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      copy.get(),
      "CREATE TABLE T (k INT64 NOT NULL, v INT64) PRIMARY KEY (k)"));
  EXPECT_THAT(Rows(copy.get()), IsOkAndHolds(IsEmpty()));
  EXPECT_THAT(Rows(base.get()), IsOkAndHolds(ElementsAre("1,10")));
}

// Copies of copies, and of a database with change streams and a column
// dropped before the copy.
TEST_F(EphemeralCopyTest, CopiesChangeStreamsAndDroppedColumns) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> base,
      Create({"CREATE TABLE T (k INT64 NOT NULL, v INT64, x INT64) "
              "PRIMARY KEY (k)",
              "CREATE CHANGE STREAM Changes FOR T"}));
  Mutation m;
  m.AddWriteOp(MutationOpType::kInsert, "T", {"k", "v", "x"},
               {{Int64(1), Int64(10), Int64(100)}});
  GOOGLESQL_ASSERT_OK(Apply(base.get(), m));
  GOOGLESQL_ASSERT_OK(UpdateSchema(base.get(), "ALTER TABLE T DROP COLUMN x"));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));
  GOOGLESQL_ASSERT_OK(Insert(copy.get(), "T", {{2, 20}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy_of_copy,
                                 Copy(copy.get()));
  EXPECT_THAT(Rows(copy_of_copy.get()),
              IsOkAndHolds(ElementsAre("1,10", "2,20")));
  GOOGLESQL_ASSERT_OK(
      UpdateSchema(copy_of_copy.get(), "ALTER TABLE T ADD COLUMN x INT64"));
  EXPECT_THAT(Sql(copy_of_copy.get(), "SELECT k, x FROM T ORDER BY k"),
              IsOkAndHolds(ElementsAre("1,NULL", "2,NULL")));
}

TEST_F(EphemeralCopyTest, CopiesPostgresqlDatabases) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> base,
      Database::Create(
          &clock_, kDatabaseId,
          SchemaChangeOperation{
              .statements = {"CREATE TABLE t (k bigint primary key, v bigint)"},
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}));
  GOOGLESQL_ASSERT_OK(Insert(base.get(), "t", {{1, 10}}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Database> copy,
                                 Copy(base.get()));
  EXPECT_EQ(copy->dialect(), database_api::DatabaseDialect::POSTGRESQL);
  // The PostgreSQL catalog names the database.
  const std::string catalog =
      "SELECT table_catalog FROM information_schema.tables "
      "WHERE table_name = 't'";
  EXPECT_THAT(Sql(copy.get(), catalog),
              IsOkAndHolds(ElementsAre("\"test-db\"")));
  GOOGLESQL_ASSERT_OK(Insert(copy.get(), "t", {{2, 20}}));
  EXPECT_EQ(copy->get_pg_oid_assigner()->next_postgresql_oid(),
            base->get_pg_oid_assigner()->next_postgresql_oid());
  GOOGLESQL_ASSERT_OK(
      UpdateSchema(copy.get(), "CREATE TABLE u (k bigint primary key)"));
  EXPECT_NE(copy->get_pg_oid_assigner()->next_postgresql_oid(),
            base->get_pg_oid_assigner()->next_postgresql_oid());
}

// A schema CopySchema cannot copy fails the copy; nothing of it remains.
TEST_F(EphemeralCopyTest, UncopyableSchemaFailsTheCopy) {
  test::ScopedEmulatorFeatureFlagsSetter flags(EmulatorFeatureFlags::Flags{
      .enable_user_defined_functions = true,
  });
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<Database> base,
      Create({R"(CREATE SEQUENCE seq OPTIONS (
                   sequence_kind = 'bit_reversed_positive'))",
              "CREATE FUNCTION f(x INT64 DEFAULT 1) RETURNS INT64 "
              "SQL SECURITY INVOKER AS (x + 1)"}));
  const size_t counters = SequenceCounterCount();
  EXPECT_THAT(base->CreateEphemeralCopy(),
              StatusIs(absl::StatusCode::kUnimplemented));
  EXPECT_EQ(SequenceCounterCount(), counters);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
