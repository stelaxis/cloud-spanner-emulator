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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_

#include <functional>
#include <memory>
#include <set>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/locking/handle.h"
#include "backend/storage/storage.h"
#include "common/clock.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// LockManager coordinates the transactions of one database. Nothing is locked:
// read-write transactions run optimistically and are validated at commit
// (SERIALIZABLE). The design is the `Target` model in verification/lean.
//
// * A read-write transaction reads a snapshot taken at its first data
//   operation (LockHandle::SnapshotTimestamp).
// * Commits run in one per-database critical section: validate the read set
//   against versions committed after the snapshot, reserve a strictly
//   increasing commit timestamp, flush, and mark the commit complete
//   (LockHandle::Commit).
// * A schema change holds the same critical section exclusively for its whole
//   duration (BeginExclusiveCommit / EndExclusiveCommit).
// * A read at a timestamp waits until every commit that reserved an earlier
//   timestamp has finished flushing (WaitForSafeRead).
class LockManager {
 public:
  // `storage` is what commit validation reads. It may be null only when no
  // handle validates a non-empty read set (lock manager unit tests).
  explicit LockManager(Clock* clock, const Storage* storage = nullptr)
      : clock_(clock), storage_(storage) {}

  // Returns a handle for a single transaction with the given id and priority.
  // Subsequent communication between the transaction and the lock manager
  // happens via the handle. See LockHandle methods for more details.
  std::unique_ptr<LockHandle> CreateHandle(TransactionID id,
                                           TransactionPriority priority);

  // Returns the timestamp at which last schema update or commit completed.
  absl::Time LastCommitTimestamp() ABSL_LOCKS_EXCLUDED(mu_);

  // Enters the commit critical section exclusively, waiting for an in-flight
  // commit to finish. Schema changes use this: they must not interleave with
  // commits, but they do not fail because read-write transactions are open.
  void BeginExclusiveCommit() ABSL_EXCLUSIVE_LOCK_FUNCTION(commit_mu_);
  void EndExclusiveCommit() ABSL_UNLOCK_FUNCTION(commit_mu_);

  // Reserves a commit timestamp greater than every timestamp handed out so far
  // and records it as pending until MarkCommitted.
  absl::Time ReserveCommitTimestamp() ABSL_EXCLUSIVE_LOCKS_REQUIRED(commit_mu_)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Marks the commit that reserved `commit_timestamp` as complete.
  void MarkCommitted(absl::Time commit_timestamp) ABSL_LOCKS_EXCLUDED(mu_);

  // Waits until `read_time` has passed and no commit with a timestamp at or
  // before `read_time` is still pending.
  void WaitForSafeRead(absl::Time read_time) ABSL_LOCKS_EXCLUDED(mu_);

 private:
  friend class LockHandle;

  // Returns a fresh timestamp at which every commit that reserved an earlier
  // timestamp has finished flushing.
  absl::Time PickSnapshotTimestamp() ABSL_LOCKS_EXCLUDED(mu_);

  // The commit critical section.
  absl::Mutex commit_mu_ ABSL_ACQUIRED_BEFORE(mu_);

  // Guards the timestamp state below.
  absl::Mutex mu_;

  // System wide monotonic clock used to provide commit and read timestamps.
  Clock* clock_;

  // Storage read by commit validation.
  const Storage* storage_;

  // Timestamp at which last schema update or commit completed.
  absl::Time last_commit_timestamp_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();

  // Timestamps of commits that reserved one and have not finished flushing.
  // Commits are serialized, so this holds at most one entry today; reads only
  // rely on its minimum.
  std::set<absl::Time> pending_commit_timestamps_ ABSL_GUARDED_BY(mu_);

  // Signals completion of a pending commit.
  absl::CondVar pending_commit_cvar_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_MANAGER_H_
