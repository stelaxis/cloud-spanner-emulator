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

#include "backend/locking/manager.h"

#include <atomic>
#include <memory>
#include <thread>  // NOLINT
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/value.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/request.h"
#include "backend/storage/in_memory_storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

using ::googlesql_base::testing::StatusIs;

constexpr char kTable[] = "table";
constexpr char kColumn[] = "value";

Key IntKey(int64_t k) { return Key({googlesql::values::Int64(k)}); }

class LockManagerTest : public testing::Test {
 public:
  Clock* clock() { return &clock_; }
  LockManager* manager() { return &manager_; }
  InMemoryStorage* storage() { return &storage_; }

  std::unique_ptr<LockHandle> Handle(int64_t id) {
    return manager_.CreateHandle(TransactionID(id), TransactionPriority(1));
  }

  // Records a read of `range` in `handle`'s read set.
  static void Read(LockHandle* handle, const KeyRange& range) {
    handle->EnqueueLock(LockRequest(LockMode::kShared, kTable, range, {}));
  }

  // Commits a write of `value` to `key` through `handle`.
  absl::StatusOr<absl::Time> CommitWrite(LockHandle* handle, int64_t key,
                                         int64_t value) {
    return handle->Commit([]() { return absl::OkStatus(); },
                          [&](absl::Time ts) {
                            return storage_.Write(
                                ts, kTable, IntKey(key), {kColumn},
                                {googlesql::values::Int64(value)});
                          });
  }

  int64_t ValueAt(absl::Time ts, int64_t key) {
    std::vector<googlesql::Value> values;
    absl::Status s = storage_.Lookup(ts, kTable, IntKey(key), {kColumn}, &values);
    return s.ok() ? values[0].int64_value() : 0;
  }

 private:
  Clock clock_;
  InMemoryStorage storage_;
  LockManager manager_ = LockManager(&clock_, &storage_);
};

TEST_F(LockManagerTest, LockRequestsNeverBlockOrAbort) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh1->EnqueueLock(LockRequest(LockMode::kExclusive, kTable, KeyRange::All(), {}));
  lh2->EnqueueLock(LockRequest(LockMode::kExclusive, kTable, KeyRange::All(), {}));
  lh1->SnapshotTimestamp();
  lh2->SnapshotTimestamp();
  EXPECT_FALSE(lh1->ReadSetIsStale());
  EXPECT_FALSE(lh2->ReadSetIsStale());
  GOOGLESQL_EXPECT_OK(CommitWrite(lh1.get(), 1, 1));
  GOOGLESQL_EXPECT_OK(CommitWrite(lh2.get(), 2, 2));
}

TEST_F(LockManagerTest, DisjointTransactionsBothCommit) {
  std::unique_ptr<LockHandle> lh1 = Handle(1);
  std::unique_ptr<LockHandle> lh2 = Handle(2);
  lh1->SnapshotTimestamp();
  lh2->SnapshotTimestamp();
  Read(lh1.get(), KeyRange::Point(IntKey(1)));
  Read(lh2.get(), KeyRange::Point(IntKey(2)));
  GOOGLESQL_EXPECT_OK(CommitWrite(lh1.get(), 1, 10));
  GOOGLESQL_EXPECT_OK(CommitWrite(lh2.get(), 2, 20));
}

TEST_F(LockManagerTest, WriteToReadKeyAbortsReader) {
  std::unique_ptr<LockHandle> reader = Handle(1);
  std::unique_ptr<LockHandle> writer = Handle(2);
  reader->SnapshotTimestamp();
  Read(reader.get(), KeyRange::Point(IntKey(1)));
  GOOGLESQL_EXPECT_OK(CommitWrite(writer.get(), 1, 10));
  EXPECT_TRUE(reader->ReadSetIsStale());
  EXPECT_THAT(CommitWrite(reader.get(), 2, 20),
              StatusIs(absl::StatusCode::kAborted));
  // Nothing was flushed for the aborted transaction.
  EXPECT_EQ(ValueAt(absl::InfiniteFuture(), 2), 0);
}

TEST_F(LockManagerTest, InsertIntoScannedRangeAbortsReader) {
  std::unique_ptr<LockHandle> reader = Handle(1);
  std::unique_ptr<LockHandle> writer = Handle(2);
  reader->SnapshotTimestamp();
  // The scan returned nothing; the range is still in the read set.
  Read(reader.get(), KeyRange::ClosedOpen(IntKey(0), IntKey(10)));
  GOOGLESQL_EXPECT_OK(CommitWrite(writer.get(), 5, 1));
  EXPECT_THAT(CommitWrite(reader.get(), 20, 1),
              StatusIs(absl::StatusCode::kAborted));
}

TEST_F(LockManagerTest, WriteOutsideScannedRangeDoesNotAbort) {
  std::unique_ptr<LockHandle> reader = Handle(1);
  std::unique_ptr<LockHandle> writer = Handle(2);
  reader->SnapshotTimestamp();
  Read(reader.get(), KeyRange::ClosedOpen(IntKey(0), IntKey(10)));
  GOOGLESQL_EXPECT_OK(CommitWrite(writer.get(), 10, 1));
  GOOGLESQL_EXPECT_OK(CommitWrite(reader.get(), 20, 1));
}

TEST_F(LockManagerTest, CommitsBeforeSnapshotDoNotAbort) {
  std::unique_ptr<LockHandle> reader = Handle(1);
  std::unique_ptr<LockHandle> writer = Handle(2);
  GOOGLESQL_EXPECT_OK(CommitWrite(writer.get(), 1, 10));
  // The snapshot is taken at the first read, after the other commit.
  reader->SnapshotTimestamp();
  Read(reader.get(), KeyRange::Point(IntKey(1)));
  GOOGLESQL_EXPECT_OK(CommitWrite(reader.get(), 1, 11));
}

TEST_F(LockManagerTest, UnlockAllForgetsSnapshotAndReadSet) {
  std::unique_ptr<LockHandle> reader = Handle(1);
  std::unique_ptr<LockHandle> writer = Handle(2);
  reader->SnapshotTimestamp();
  Read(reader.get(), KeyRange::Point(IntKey(1)));
  GOOGLESQL_EXPECT_OK(CommitWrite(writer.get(), 1, 10));
  reader->UnlockAll();
  EXPECT_FALSE(reader->snapshot().has_value());
  EXPECT_FALSE(reader->ReadSetIsStale());
  GOOGLESQL_EXPECT_OK(CommitWrite(reader.get(), 2, 1));
}

TEST_F(LockManagerTest, PrecheckErrorIsReturned) {
  std::unique_ptr<LockHandle> lh = Handle(1);
  EXPECT_THAT(
      lh->Commit([]() { return absl::FailedPreconditionError("schema"); },
                 [](absl::Time) { return absl::OkStatus(); }),
      StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(LockManagerTest, CommitTimestampsStrictlyIncrease) {
  absl::Time last = absl::InfinitePast();
  for (int i = 0; i < 100; ++i) {
    std::unique_ptr<LockHandle> lh = Handle(i + 1);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time ts, CommitWrite(lh.get(), i, i));
    EXPECT_GT(ts, last);
    last = ts;
  }
  EXPECT_EQ(manager()->LastCommitTimestamp(), last);
}

TEST_F(LockManagerTest, SnapshotWaitsForCommitInFlight) {
  std::unique_ptr<LockHandle> writer = Handle(1);
  absl::Notification flushing, release;
  absl::Time commit_ts;
  std::thread commit([&]() {
    commit_ts = *writer->Commit([]() { return absl::OkStatus(); },
                                [&](absl::Time ts) {
                                  flushing.Notify();
                                  release.WaitForNotification();
                                  return storage()->Write(
                                      ts, kTable, IntKey(1), {kColumn},
                                      {googlesql::values::Int64(7)});
                                });
  });
  flushing.WaitForNotification();

  // A snapshot taken now is after the reserved commit timestamp, so it must
  // not be handed out until the commit has flushed.
  std::unique_ptr<LockHandle> reader = Handle(2);
  std::atomic<bool> snapshot_taken = false;
  absl::Time snapshot;
  std::thread read([&]() {
    snapshot = reader->SnapshotTimestamp();
    snapshot_taken = true;
  });
  absl::SleepFor(absl::Milliseconds(50));
  EXPECT_FALSE(snapshot_taken);
  release.Notify();
  commit.join();
  read.join();
  EXPECT_GT(snapshot, commit_ts);
  EXPECT_EQ(ValueAt(snapshot, 1), 7);
}

// A read at exactly a pending commit's timestamp waits for the whole commit:
// the commit pauses between its two row writes, and the reader must see both
// rows, never one.
TEST_F(LockManagerTest, ReadAtPendingCommitTimestampSeesWholeCommit) {
  std::unique_ptr<LockHandle> writer = Handle(1);
  absl::Notification first_row_written, release;
  absl::Time commit_ts;
  std::thread commit([&]() {
    ASSERT_TRUE(writer
                    ->Commit([]() { return absl::OkStatus(); },
                             [&](absl::Time ts) -> absl::Status {
                               absl::Status s = storage()->Write(
                                   ts, kTable, IntKey(1), {kColumn},
                                   {googlesql::values::Int64(1)});
                               if (!s.ok()) return s;
                               commit_ts = ts;
                               first_row_written.Notify();
                               release.WaitForNotification();
                               return storage()->Write(
                                   ts, kTable, IntKey(2), {kColumn},
                                   {googlesql::values::Int64(2)});
                             })
                    .ok());
  });
  first_row_written.WaitForNotification();

  absl::Notification read_done;
  int64_t rows_seen = 0;
  std::thread read([&]() {
    manager()->WaitForSafeRead(commit_ts);
    rows_seen = (ValueAt(commit_ts, 1) != 0) + (ValueAt(commit_ts, 2) != 0);
    read_done.Notify();
  });
  // Without the wait, the reader finishes here having seen one row.
  read_done.WaitForNotificationWithTimeout(absl::Milliseconds(200));
  release.Notify();
  commit.join();
  read.join();
  EXPECT_EQ(rows_seen, 2);
}

TEST_F(LockManagerTest, ExclusiveCommitSectionBlocksCommits) {
  manager()->BeginExclusiveCommit();
  absl::Time schema_ts = manager()->ReserveCommitTimestamp();
  std::unique_ptr<LockHandle> lh = Handle(1);
  std::atomic<bool> committed = false;
  absl::Time commit_ts;
  std::thread commit([&]() {
    commit_ts = *CommitWrite(lh.get(), 1, 1);
    committed = true;
  });
  absl::SleepFor(absl::Milliseconds(50));
  EXPECT_FALSE(committed);
  manager()->MarkCommitted(schema_ts);
  manager()->EndExclusiveCommit();
  commit.join();
  EXPECT_GT(commit_ts, schema_ts);
}

TEST_F(LockManagerTest, EnsuresSerializationWithParallelTransactions) {
  // n threads each increment one counter k times with optimistic retries.
  // Lost updates would leave the counter below n*k.
  const int n = 16;
  const int k = 25;
  std::vector<std::thread> threads;
  std::atomic<int> id_counter(0);
  std::atomic<int> aborts(0);
  for (int i = 0; i < n; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < k; ++j) {
        while (true) {
          std::unique_ptr<LockHandle> lh = Handle(++id_counter);
          absl::Time snapshot = lh->SnapshotTimestamp();
          Read(lh.get(), KeyRange::Point(IntKey(0)));
          int64_t value = ValueAt(snapshot, 0);
          absl::StatusOr<absl::Time> status = CommitWrite(lh.get(), 0, value + 1);
          if (status.ok()) {
            break;
          }
          ASSERT_EQ(status.status().code(), absl::StatusCode::kAborted);
          ++aborts;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(ValueAt(absl::InfiniteFuture(), 0), n * k);
}

}  // namespace

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
