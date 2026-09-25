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

#include <memory>

#include "absl/memory/memory.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

std::unique_ptr<LockHandle> LockManager::CreateHandle(
    TransactionID tid, TransactionPriority priority) {
  return absl::WrapUnique(new LockHandle(this, tid, priority));
}

void LockManager::BeginExclusiveCommit() { commit_mu_.lock(); }

void LockManager::EndExclusiveCommit() { commit_mu_.unlock(); }

absl::Time LockManager::ReserveCommitTimestamp() {
  absl::MutexLock lock(mu_);
  absl::Time commit_timestamp = clock_->Now();
  pending_commit_timestamps_.insert(commit_timestamp);
  return commit_timestamp;
}

void LockManager::MarkCommitted(absl::Time commit_timestamp) {
  absl::MutexLock lock(mu_);
  pending_commit_timestamps_.erase(commit_timestamp);
  if (last_commit_timestamp_ < commit_timestamp) {
    last_commit_timestamp_ = commit_timestamp;
  }
  pending_commit_cvar_.SignalAll();
}

void LockManager::WaitForSafeRead(absl::Time read_time) {
  absl::MutexLock lock(mu_);

  // Wait for read time to become current if passed a future timestamp  for the
  // case of exact timestamp bound for snapshot read.
  // https://cloud.google.com/spanner/docs/timestamp-bounds#introduction
  bool f = false;
  mu_.AwaitWithDeadline(absl::Condition(&f), read_time);

  // A read at a pending commit's own timestamp would see the commit's versions
  // as they are flushed, so it waits for that commit too.
  while (!pending_commit_timestamps_.empty() &&
         *pending_commit_timestamps_.begin() <= read_time) {
    pending_commit_cvar_.Wait(&mu_);
  }
}

absl::Time LockManager::PickSnapshotTimestamp() {
  // Every commit that reserves a timestamp after this one is invisible at it;
  // every commit that reserved an earlier one is waited for.
  absl::Time snapshot_timestamp = clock_->Now();
  WaitForSafeRead(snapshot_timestamp);
  return snapshot_timestamp;
}

absl::Time LockManager::LastCommitTimestamp() {
  absl::ReaderMutexLock lock(mu_);
  return last_commit_timestamp_;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
