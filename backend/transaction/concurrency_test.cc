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

// Concurrent read-write transactions: optimistic, SERIALIZABLE, validated at
// commit. The cases mirror the Lean `Target` model (verification/lean).

#include <atomic>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/actions/manager.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/datamodel/value.h"
#include "backend/locking/manager.h"
#include "backend/query/function_catalog.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "tests/common/schema_constructor.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql_base::testing::StatusIs;

class ConcurrencyTest : public testing::Test {
 public:
  void SetUp() override {
    type_factory_ = std::make_unique<googlesql::TypeFactory>();
    storage_ = std::make_unique<InMemoryStorage>();
    lock_manager_ = std::make_unique<LockManager>(&clock_, storage_.get());
    versioned_catalog_ = std::make_unique<VersionedCatalog>(
        test::CreateSchemaFromDDL({R"sql(
                CREATE TABLE T (
                  k INT64 NOT NULL,
                  v INT64
                ) PRIMARY KEY (k)
              )sql",
                                   R"sql(
                CREATE TABLE D (
                  k INT64 NOT NULL DEFAULT (7),
                  v INT64
                ) PRIMARY KEY (k)
              )sql"},
                                  type_factory_.get())
            .value());
    function_catalog_ = std::make_unique<FunctionCatalog>(
        type_factory_.get(), kCloudSpannerEmulatorFunctionCatalogName,
        versioned_catalog_->GetLatestSchema());
    action_manager_ = std::make_unique<ActionManager>();
    action_manager_->AddActionsForSchema(
        versioned_catalog_->GetSchema(absl::InfiniteFuture()),
        function_catalog_.get(), type_factory_.get());
  }

 protected:
  std::unique_ptr<ReadWriteTransaction> Begin() {
    return std::make_unique<ReadWriteTransaction>(
        ReadWriteOptions(), RetryState(), ++id_counter_, &clock_,
        storage_.get(), lock_manager_.get(), versioned_catalog_.get(),
        action_manager_.get());
  }

  static Mutation Op(MutationOpType type, int64_t k, int64_t v) {
    Mutation m;
    m.AddWriteOp(type, "T", {"k", "v"}, {{Int64(k), Int64(v)}});
    return m;
  }
  static Mutation Insert(int64_t k, int64_t v) {
    return Op(MutationOpType::kInsert, k, v);
  }
  static Mutation Update(int64_t k, int64_t v) {
    return Op(MutationOpType::kUpdate, k, v);
  }
  static Mutation Delete(int64_t k) {
    Mutation m;
    m.AddDeleteOp("T", KeySet(Key({Int64(k)})));
    return m;
  }

  // Reads (k, v) rows of `key_set` in `txn`.
  static absl::StatusOr<std::vector<std::pair<int64_t, int64_t>>> Read(
      ReadWriteTransaction* txn, const KeySet& key_set) {
    ReadArg read_arg{.table = "T", .key_set = key_set, .columns = {"k", "v"}};
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_RETURN_IF_ERROR(txn->Read(read_arg, &cursor));
    std::vector<std::pair<int64_t, int64_t>> rows;
    while (cursor->Next()) {
      rows.emplace_back(cursor->ColumnValue(0).int64_value(),
                        cursor->ColumnValue(1).int64_value());
    }
    GOOGLESQL_RETURN_IF_ERROR(cursor->Status());
    return rows;
  }
  static absl::StatusOr<std::vector<std::pair<int64_t, int64_t>>> ReadKey(
      ReadWriteTransaction* txn, int64_t k) {
    return Read(txn, KeySet(Key({Int64(k)})));
  }

  // Commits `m` in a fresh transaction.
  void Seed(const Mutation& m) {
    auto txn = Begin();
    GOOGLESQL_ASSERT_OK(txn->Write(m));
    GOOGLESQL_ASSERT_OK(txn->Commit());
  }

  absl::StatusOr<std::vector<std::pair<int64_t, int64_t>>> ReadAllStrong() {
    auto txn = Begin();
    return Read(txn.get(), KeySet::All());
  }

  Clock clock_;
  std::unique_ptr<googlesql::TypeFactory> type_factory_;
  std::unique_ptr<InMemoryStorage> storage_;
  std::unique_ptr<LockManager> lock_manager_;
  std::unique_ptr<VersionedCatalog> versioned_catalog_;
  std::unique_ptr<FunctionCatalog> function_catalog_;
  std::unique_ptr<ActionManager> action_manager_;
  std::atomic<int> id_counter_ = 0;
};

using Rows = std::vector<std::pair<int64_t, int64_t>>;

// Lean `lateReader`: the snapshot is taken at the first data operation, not
// at begin, so a commit between begin and the first read is visible and does
// not abort the reader.
TEST_F(ConcurrencyTest, SnapshotIsTakenAtFirstOperation) {
  Seed(Insert(1, 10));
  auto t1 = Begin();
  auto t2 = Begin();
  GOOGLESQL_ASSERT_OK(t2->Write(Update(1, 11)));
  GOOGLESQL_ASSERT_OK(t2->Commit());

  EXPECT_THAT(ReadKey(t1.get(), 1),
              googlesql_base::testing::IsOkAndHolds(Rows{{1, 11}}));
  GOOGLESQL_ASSERT_OK(t1->Write(Update(1, 12)));
  GOOGLESQL_EXPECT_OK(t1->Commit());
}

TEST_F(ConcurrencyTest, ReadsSeeTheSnapshotNotLaterCommits) {
  Seed(Insert(1, 10));
  auto t1 = Begin();
  EXPECT_THAT(ReadKey(t1.get(), 1),
              googlesql_base::testing::IsOkAndHolds(Rows{{1, 10}}));
  Seed(Update(1, 11));
  EXPECT_THAT(ReadKey(t1.get(), 1),
              googlesql_base::testing::IsOkAndHolds(Rows{{1, 10}}));
}

TEST_F(ConcurrencyTest, DisjointTransactionsBothCommit) {
  Seed(Insert(1, 0));
  Seed(Insert(2, 0));
  auto t1 = Begin();
  auto t2 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 1));
  GOOGLESQL_ASSERT_OK(ReadKey(t2.get(), 2));
  GOOGLESQL_ASSERT_OK(t1->Write(Update(1, 1)));
  GOOGLESQL_ASSERT_OK(t2->Write(Update(2, 2)));
  GOOGLESQL_EXPECT_OK(t2->Commit());
  GOOGLESQL_EXPECT_OK(t1->Commit());
  EXPECT_THAT(ReadAllStrong(),
              googlesql_base::testing::IsOkAndHolds(Rows{{1, 1}, {2, 2}}));
}

// Lost update: both read the row, both write it; the second commit aborts.
TEST_F(ConcurrencyTest, ConflictingReadModifyWriteAborts) {
  Seed(Insert(1, 0));
  auto t1 = Begin();
  auto t2 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 1));
  GOOGLESQL_ASSERT_OK(ReadKey(t2.get(), 1));
  GOOGLESQL_ASSERT_OK(t1->Write(Update(1, 1)));
  GOOGLESQL_ASSERT_OK(t2->Write(Update(1, 2)));
  GOOGLESQL_EXPECT_OK(t1->Commit());
  EXPECT_THAT(t2->Commit(), StatusIs(absl::StatusCode::kAborted));
  EXPECT_THAT(ReadAllStrong(),
              googlesql_base::testing::IsOkAndHolds(Rows{{1, 1}}));
}

// Write skew: each reads both rows and writes a different one. Under
// SERIALIZABLE the second commit must abort.
TEST_F(ConcurrencyTest, WriteSkewAborts) {
  Seed(Insert(1, 1));
  Seed(Insert(2, 1));
  auto t1 = Begin();
  auto t2 = Begin();
  KeySet both;
  both.AddKey(Key({Int64(1)}));
  both.AddKey(Key({Int64(2)}));
  GOOGLESQL_ASSERT_OK(Read(t1.get(), both));
  GOOGLESQL_ASSERT_OK(Read(t2.get(), both));
  GOOGLESQL_ASSERT_OK(t1->Write(Update(1, 0)));
  GOOGLESQL_ASSERT_OK(t2->Write(Update(2, 0)));
  GOOGLESQL_EXPECT_OK(t1->Commit());
  EXPECT_THAT(t2->Commit(), StatusIs(absl::StatusCode::kAborted));
}

// Lean `phantom`: the read set holds the scanned range, not the rows it
// returned, so an insert into the range aborts the scanner.
TEST_F(ConcurrencyTest, PhantomInsertIntoScannedRangeAborts) {
  Seed(Insert(1, 10));
  auto t1 = Begin();
  EXPECT_THAT(
      Read(t1.get(),
           KeySet(KeyRange::ClosedClosed(Key({Int64(0)}), Key({Int64(3)})))),
      googlesql_base::testing::IsOkAndHolds(Rows{{1, 10}}));
  Seed(Insert(2, 20));
  GOOGLESQL_ASSERT_OK(t1->Write(Insert(9, 1)));
  EXPECT_THAT(t1->Commit(), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, InsertOutsideScannedRangeDoesNotAbort) {
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(Read(
      t1.get(), KeySet(KeyRange::ClosedOpen(Key({Int64(0)}), Key({Int64(3)})))));
  Seed(Insert(3, 30));
  GOOGLESQL_ASSERT_OK(t1->Write(Insert(9, 1)));
  GOOGLESQL_EXPECT_OK(t1->Commit());
}

// Lean `naive_error_not_latest`: ALREADY_EXISTS against a snapshot whose row
// has since been deleted is reported as ABORTED.
TEST_F(ConcurrencyTest, StaleConstraintErrorIsAborted) {
  Seed(Insert(1, 10));
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 7));
  Seed(Delete(1));
  EXPECT_THAT(t1->Write(Insert(1, 7)), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, FreshConstraintErrorIsReported) {
  Seed(Insert(1, 10));
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 7));
  EXPECT_THAT(t1->Write(Insert(1, 7)),
              StatusIs(absl::StatusCode::kAlreadyExists));
}

// A commit's mutations are validated together: an error from one mutation is
// ABORTED if a later mutation's row changed after the snapshot.
TEST_F(ConcurrencyTest, MutationErrorValidatesEveryMutationRow) {
  Seed(Insert(2, 20));
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 9));
  Seed(Update(2, 21));
  Mutation m = Update(3, 0);  // Row 3 does not exist: NOT_FOUND.
  m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
               {{Int64(2), Int64(22)}});
  EXPECT_THAT(t1->Write(m), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, MutationErrorWithFreshReadsIsReported) {
  Seed(Insert(2, 20));
  auto t1 = Begin();
  Mutation m = Update(3, 0);
  m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
               {{Int64(2), Int64(22)}});
  EXPECT_THAT(t1->Write(m), StatusIs(absl::StatusCode::kNotFound));
}

// A mutation whose key has a default value cannot be put in the read set
// before the mutations are applied. If an earlier mutation fails first, the
// whole table stands in for that row: a commit to it after the snapshot turns
// the error into ABORTED, as validating the row itself would have.
TEST_F(ConcurrencyTest, MutationErrorValidatesRowsWithDefaultKeys) {
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 9));
  auto t2 = Begin();
  Mutation insert_default;
  insert_default.AddWriteOp(MutationOpType::kInsert, "D", {"v"},
                            {{Int64(1)}});
  GOOGLESQL_ASSERT_OK(t2->Write(insert_default));
  GOOGLESQL_ASSERT_OK(t2->Commit());

  Mutation m = Update(3, 0);  // Row 3 does not exist: NOT_FOUND.
  m.AddWriteOp(MutationOpType::kInsert, "D", {"v"}, {{Int64(2)}});
  EXPECT_THAT(t1->Write(m), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, MutationErrorWithDefaultKeysAndFreshReadsIsReported) {
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 9));
  Mutation m = Update(3, 0);
  m.AddWriteOp(MutationOpType::kInsert, "D", {"v"}, {{Int64(2)}});
  EXPECT_THAT(t1->Write(m), StatusIs(absl::StatusCode::kNotFound));
}

// Inserting and then deleting an absent row buffers nothing, but it is still a
// write that readers of the row must see as a conflict.
TEST_F(ConcurrencyTest, CancelledWriteConflictsWithReaders) {
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 5));
  auto t2 = Begin();
  Mutation m = Insert(5, 1);
  m.AddDeleteOp("T", KeySet(Key({Int64(5)})));
  GOOGLESQL_ASSERT_OK(t2->Write(m));
  GOOGLESQL_ASSERT_OK(t2->Commit());
  EXPECT_THAT(ReadAllStrong(), googlesql_base::testing::IsOkAndHolds(Rows{}));
  GOOGLESQL_ASSERT_OK(t1->Write(Insert(6, 1)));
  EXPECT_THAT(t1->Commit(), StatusIs(absl::StatusCode::kAborted));
}

// Lean `blind`: mutations read their row while flattening, so two upserts of
// one row conflict.
TEST_F(ConcurrencyTest, ConcurrentUpsertsOfOneRowConflict) {
  auto t1 = Begin();
  auto t2 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 8));
  GOOGLESQL_ASSERT_OK(ReadKey(t2.get(), 9));
  GOOGLESQL_ASSERT_OK(t1->Write(Op(MutationOpType::kInsertOrUpdate, 1, 1)));
  GOOGLESQL_ASSERT_OK(t2->Write(Op(MutationOpType::kInsertOrUpdate, 1, 2)));
  GOOGLESQL_EXPECT_OK(t1->Commit());
  EXPECT_THAT(t2->Commit(), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, CommitAfterSchemaChangeAborts) {
  auto t1 = Begin();
  GOOGLESQL_ASSERT_OK(ReadKey(t1.get(), 1));
  GOOGLESQL_ASSERT_OK(t1->Write(Insert(1, 1)));
  GOOGLESQL_ASSERT_OK(versioned_catalog_->AddSchema(
      clock_.Now(), test::CreateSchemaFromDDL({R"sql(
                CREATE TABLE T (
                  k INT64 NOT NULL,
                  v INT64
                ) PRIMARY KEY (k)
              )sql",
                                               R"sql(
                CREATE TABLE U (k INT64 NOT NULL) PRIMARY KEY (k)
              )sql"},
                                              type_factory_.get())
                        .value()));
  EXPECT_THAT(t1->Commit(), StatusIs(absl::StatusCode::kAborted));
}

TEST_F(ConcurrencyTest, CommitTimestampsIncreaseAcrossTransactions) {
  absl::Time last = absl::InfinitePast();
  for (int i = 0; i < 20; ++i) {
    auto txn = Begin();
    GOOGLESQL_ASSERT_OK(txn->Write(Insert(i, i)));
    GOOGLESQL_ASSERT_OK(txn->Commit());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time ts, txn->GetCommitTimestamp());
    EXPECT_GT(ts, last);
    last = ts;
  }
}

// Lean `disjoint_never_abort`: transactions on disjoint rows never abort.
TEST_F(ConcurrencyTest, DisjointWorkloadNeverAborts) {
  constexpr int kThreads = 16;
  constexpr int kIncrements = 50;
  for (int t = 0; t < kThreads; ++t) {
    Seed(Insert(t, 0));
  }
  std::atomic<int> aborts = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kIncrements; ++i) {
        auto txn = Begin();
        absl::StatusOr<Rows> rows = ReadKey(txn.get(), t);
        absl::Status status = rows.status();
        if (status.ok()) {
          status = txn->Write(Update(t, (*rows)[0].second + 1));
        }
        if (status.ok()) {
          status = txn->Commit();
        }
        if (!status.ok()) {
          ++aborts;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(aborts, 0);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(Rows rows, ReadAllStrong());
  ASSERT_EQ(rows.size(), kThreads);
  for (const auto& [k, v] : rows) {
    EXPECT_EQ(v, kIncrements) << "row " << k;
  }
}

// Bank transfers between a few hot accounts, with retries: the total is
// invariant and no update is lost.
TEST_F(ConcurrencyTest, ContendedTransfersPreserveTheTotal) {
  constexpr int kAccounts = 4;
  constexpr int kThreads = 16;
  constexpr int kTransfers = 30;
  for (int a = 0; a < kAccounts; ++a) {
    Seed(Insert(a, 1000));
  }
  std::atomic<int> committed = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kTransfers; ++i) {
        int from = (t + i) % kAccounts;
        int to = (t + 2 * i + 1) % kAccounts;
        if (from == to) to = (to + 1) % kAccounts;
        while (true) {
          auto txn = Begin();
          absl::StatusOr<Rows> a = ReadKey(txn.get(), from);
          absl::StatusOr<Rows> b = ReadKey(txn.get(), to);
          ASSERT_TRUE(a.ok() && b.ok());
          Mutation m = Update(from, (*a)[0].second - 1);
          m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
                       {{Int64(to), Int64((*b)[0].second + 1)}});
          absl::Status status = txn->Write(m);
          if (status.ok()) {
            status = txn->Commit();
          }
          if (status.ok()) {
            ++committed;
            break;
          }
          ASSERT_EQ(status.code(), absl::StatusCode::kAborted) << status;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(committed, kThreads * kTransfers);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(Rows rows, ReadAllStrong());
  int64_t total = 0;
  for (const auto& [k, v] : rows) total += v;
  EXPECT_EQ(total, kAccounts * 1000);
}

// A strong read-only transaction never observes half of a commit.
TEST_F(ConcurrencyTest, StrongReadOnlyReadsSeeWholeCommits) {
  Seed(Insert(1, 0));
  Seed(Insert(2, 0));
  std::atomic<bool> done = false;
  std::thread writer([&]() {
    for (int i = 1; i <= 200; ++i) {
      auto txn = Begin();
      Mutation m = Update(1, i);
      m.AddWriteOp(MutationOpType::kUpdate, "T", {"k", "v"},
                   {{Int64(2), Int64(i)}});
      ASSERT_TRUE(txn->Write(m).ok());
      ASSERT_TRUE(txn->Commit().ok());
    }
    done = true;
  });
  while (!done) {
    ReadOnlyTransaction ro(ReadOnlyOptions(), ++id_counter_, &clock_,
                           storage_.get(), lock_manager_.get(),
                           versioned_catalog_.get());
    ReadArg read_arg{.table = "T", .key_set = KeySet::All(),
                     .columns = {"k", "v"}};
    std::unique_ptr<RowCursor> cursor;
    GOOGLESQL_ASSERT_OK(ro.Read(read_arg, &cursor));
    std::vector<int64_t> values;
    while (cursor->Next()) values.push_back(cursor->ColumnValue(1).int64_value());
    ASSERT_EQ(values.size(), 2);
    EXPECT_EQ(values[0], values[1]);
  }
  writer.join();
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
