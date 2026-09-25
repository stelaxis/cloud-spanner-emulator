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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_SCOPED_SCHEMA_CHANGE_LOCK_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_SCOPED_SCHEMA_CHANGE_LOCK_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/locking/manager.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Holds the database's commit critical section exclusively for the lifetime of
// a schema change. Waits for an in-flight commit to finish; open read-write
// transactions do not block it. They abort on their next operation, or at
// commit, once they see the new schema.
class ScopedSchemaChangeLock {
 public:
  ScopedSchemaChangeLock(TransactionID tid, LockManager* lock_manager)
      : lock_manager_(lock_manager) {
    lock_manager_->BeginExclusiveCommit();
  }

  ScopedSchemaChangeLock(const ScopedSchemaChangeLock&) = delete;
  ScopedSchemaChangeLock& operator=(const ScopedSchemaChangeLock&) = delete;

  // Kept for callers that expect a fallible acquisition; always OK.
  absl::Status Wait() { return absl::OkStatus(); }

  // Reserves a commit timestamp for the schema change.
  absl::StatusOr<absl::Time> ReserveCommitTimestamp() {
    commit_timestamp_ = lock_manager_->ReserveCommitTimestamp();
    has_commit_timestamp_ = true;
    return commit_timestamp_;
  }

  ~ScopedSchemaChangeLock() {
    if (has_commit_timestamp_) {
      lock_manager_->MarkCommitted(commit_timestamp_);
    }
    lock_manager_->EndExclusiveCommit();
  }

 private:
  LockManager* lock_manager_;

  bool has_commit_timestamp_ = false;
  absl::Time commit_timestamp_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_UPDATER_SCOPED_SCHEMA_CHANGE_LOCK_H_
