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

#include "backend/locking/handle.h"

#include <functional>
#include <optional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/locking/manager.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

LockHandle::LockHandle(LockManager* manager, TransactionID tid,
                       TransactionPriority priority)
    : manager_(manager), tid_(tid), priority_(priority) {}

void LockHandle::EnqueueLock(const LockRequest& request) {
  if (request.mode() != LockMode::kShared) {
    return;
  }
  KeyRange range = request.key_range().IsClosedOpen()
                       ? request.key_range()
                       : request.key_range().ToClosedOpen();
  // An empty range reads nothing.
  if (range.start_key() >= range.limit_key()) {
    return;
  }
  absl::MutexLock lock(mu_);
  TableReads& reads = read_set_[request.table_id()];
  if (reads.all) {
    return;
  }
  if (range.start_key() == Key::Empty() && range.limit_key() == Key::Infinity()) {
    reads.all = true;
    reads.ranges.clear();
    return;
  }
  // Validators look the same row up repeatedly; skip exact repeats.
  if (!reads.ranges.empty() && reads.ranges.back() == range) {
    return;
  }
  reads.ranges.push_back(std::move(range));
}

void LockHandle::UnlockAll() {
  absl::MutexLock lock(mu_);
  snapshot_.reset();
  read_set_.clear();
}

absl::Time LockHandle::SnapshotTimestamp() {
  absl::MutexLock lock(mu_);
  if (!snapshot_.has_value()) {
    snapshot_ = manager_->PickSnapshotTimestamp();
  }
  return *snapshot_;
}

std::optional<absl::Time> LockHandle::snapshot() {
  absl::MutexLock lock(mu_);
  return snapshot_;
}

bool LockHandle::ReadSetIsStale() {
  absl::MutexLock lock(mu_);
  return ReadSetIsStaleLocked();
}

bool LockHandle::TableChangedSinceSnapshot(const TableID& table_id) {
  absl::MutexLock lock(mu_);
  const Storage* storage = manager_->storage_;
  return snapshot_.has_value() && storage != nullptr &&
         storage->HasVersionsAfter(*snapshot_, table_id, KeyRange::All());
}

bool LockHandle::ReadSetIsStaleLocked() {
  if (!snapshot_.has_value() || read_set_.empty()) {
    return false;
  }
  const Storage* storage = manager_->storage_;
  if (storage == nullptr) {
    return false;
  }
  for (const auto& [table_id, reads] : read_set_) {
    if (reads.all) {
      if (storage->HasVersionsAfter(*snapshot_, table_id, KeyRange::All())) {
        return true;
      }
      continue;
    }
    for (const KeyRange& range : reads.ranges) {
      if (storage->HasVersionsAfter(*snapshot_, table_id, range)) {
        return true;
      }
    }
  }
  return false;
}

absl::StatusOr<absl::Time> LockHandle::Commit(
    const std::function<absl::Status()>& precheck,
    const std::function<absl::Status(absl::Time)>& flush) {
  absl::MutexLock commit_lock(manager_->commit_mu_);
  GOOGLESQL_RETURN_IF_ERROR(precheck());
  {
    absl::MutexLock lock(mu_);
    if (ReadSetIsStaleLocked()) {
      return error::AbortReadSetConflict(tid_);
    }
  }
  absl::Time commit_timestamp = manager_->ReserveCommitTimestamp();
  absl::Status flush_status = flush(commit_timestamp);
  manager_->MarkCommitted(commit_timestamp);
  GOOGLESQL_RETURN_IF_ERROR(flush_status);
  return commit_timestamp;
}

void LockHandle::WaitForSafeRead(absl::Time read_time) {
  manager_->WaitForSafeRead(read_time);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
