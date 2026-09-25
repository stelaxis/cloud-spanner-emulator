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

#include "backend/storage/recording_storage.h"

#include <memory>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

absl::Status ApplyStorageOps(absl::Time timestamp,
                             const std::vector<StorageOp>& ops,
                             Storage* storage) {
  for (const StorageOp& op : ops) {
    absl::Status status;
    if (const auto* write = std::get_if<StorageWrite>(&op)) {
      status = storage->Write(timestamp, write->table_id, write->key,
                              write->column_ids, write->values);
    } else {
      const auto& del = std::get<StorageDelete>(op);
      status = storage->Delete(timestamp, del.table_id, del.key_range);
    }
    if (!status.ok()) return status;
  }
  return absl::OkStatus();
}

namespace {

struct RecordingSavepoint : public StorageSavepoint {
  std::unique_ptr<StorageSavepoint> base;
  size_t num_ops;
};

}  // namespace

std::unique_ptr<StorageSavepoint> RecordingStorage::SaveVersionsAt(
    absl::Time timestamp) {
  auto savepoint = std::make_unique<RecordingSavepoint>();
  savepoint->base = base_->SaveVersionsAt(timestamp);
  savepoint->num_ops = ops_.size();
  return savepoint;
}

void RecordingStorage::RestoreVersionsAt(absl::Time timestamp,
                                         const StorageSavepoint& savepoint) {
  const auto& recorded = static_cast<const RecordingSavepoint&>(savepoint);
  base_->RestoreVersionsAt(timestamp, *recorded.base);
  ops_.erase(ops_.begin() + recorded.num_ops, ops_.end());
}

absl::Status RecordingStorage::Write(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    const std::vector<googlesql::Value>& values) {
  ops_.push_back(StorageWrite{table_id, key, column_ids, values});
  if (!forward_) return absl::OkStatus();
  return base_->Write(timestamp, table_id, key, column_ids, values);
}

absl::Status RecordingStorage::Delete(absl::Time timestamp,
                                      const TableID& table_id,
                                      const KeyRange& key_range) {
  ops_.push_back(StorageDelete{table_id, key_range});
  if (!forward_) return absl::OkStatus();
  return base_->Delete(timestamp, table_id, key_range);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
