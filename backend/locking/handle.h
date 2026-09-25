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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_HANDLE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_HANDLE_H_

#include <functional>
#include <optional>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/request.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

class LockManager;

// A transaction's view of the LockManager: its snapshot timestamp, its read
// set, and the commit protocol. Methods are thread-safe.
class LockHandle {
 public:
  // Returns the ID of the transaction which owns this handle.
  TransactionID tid() { return tid_; }

  // Returns the priority of the transaction which owns this handle.
  TransactionPriority priority() { return priority_; }

  // Records a lock request. Nothing is locked and the call never blocks or
  // aborts. A kShared request adds its key range to the read set that Commit
  // validates. The key range is what the read *scanned*, not the rows it
  // returned, so a later insert into the range is a conflict (no phantoms).
  // Row granularity: columns are ignored. kExclusive requests are not needed
  // for validation, which reads committed versions from storage.
  void EnqueueLock(const LockRequest& request) ABSL_LOCKS_EXCLUDED(mu_);

  // Forgets the snapshot and the read set. The transaction can start over.
  void UnlockAll() ABSL_LOCKS_EXCLUDED(mu_);

  // Returns the transaction's snapshot timestamp, fixing it on the first call.
  // The snapshot is taken lazily, at the transaction's first data operation
  // rather than at BeginTransaction, so a transaction that begins, waits, and
  // then reads does not validate against commits it could have observed.
  absl::Time SnapshotTimestamp() ABSL_LOCKS_EXCLUDED(mu_);

  // Returns the snapshot timestamp if one has been taken.
  std::optional<absl::Time> snapshot() ABSL_LOCKS_EXCLUDED(mu_);

  // Returns true if a version committed after the snapshot lies in a key range
  // of the read set. False when no snapshot was taken.
  bool ReadSetIsStale() ABSL_LOCKS_EXCLUDED(mu_);

  // Returns true if a version committed after the snapshot lies anywhere in
  // the table. False when no snapshot was taken.
  bool TableChangedSinceSnapshot(const TableID& table_id)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Runs the commit protocol in the database's commit critical section:
  //   1. `precheck` (e.g. the schema is unchanged); its error is returned.
  //   2. Validate the read set; ABORTED if stale.
  //   3. Reserve a commit timestamp.
  //   4. `flush` at that timestamp, then mark the commit complete.
  // Returns the commit timestamp, or the first error. Validation and timestamp
  // reservation are atomic with respect to every other commit.
  absl::StatusOr<absl::Time> Commit(
      const std::function<absl::Status()>& precheck,
      const std::function<absl::Status(absl::Time)>& flush)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Waits for the intended read timestamp to be safe from any in-progress
  // commits.
  void WaitForSafeRead(absl::Time read_time);

 private:
  // Only the LockManager is allowed to create and destroy LockHandles.
  friend class LockManager;
  friend std::unique_ptr<LockHandle>::deleter_type;
  LockHandle(LockManager* manager, TransactionID tid,
             TransactionPriority priority);
  ~LockHandle() = default;

  // Key ranges read from one table. `all` subsumes `ranges`.
  struct TableReads {
    bool all = false;
    std::vector<KeyRange> ranges;
  };

  bool ReadSetIsStaleLocked() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  // The LockManager which this LockHandle interacts with.
  LockManager* const manager_;

  // The ID of the transaction which owns this lock handle.
  TransactionID tid_;

  // The priority of the transaction which owns this lock handle.
  TransactionPriority priority_;

  // Mutex to guard state below.
  absl::Mutex mu_;

  // The snapshot timestamp, once taken.
  std::optional<absl::Time> snapshot_ ABSL_GUARDED_BY(mu_);

  // The read set: ClosedOpen key ranges per table.
  absl::flat_hash_map<TableID, TableReads> read_set_ ABSL_GUARDED_BY(mu_);
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_LOCKING_HANDLE_H_
