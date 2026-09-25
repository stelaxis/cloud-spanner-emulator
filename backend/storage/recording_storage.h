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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_RECORDING_STORAGE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_RECORDING_STORAGE_H_

#include <memory>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "backend/common/ids.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/storage/iterator.h"
#include "backend/storage/storage.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// A Storage::Write, with its values resolved.
struct StorageWrite {
  TableID table_id;
  Key key;
  std::vector<ColumnID> column_ids;
  std::vector<googlesql::Value> values;
};

// A Storage::Delete of a ClosedOpen range.
struct StorageDelete {
  TableID table_id;
  KeyRange key_range;
};

using StorageOp = std::variant<StorageWrite, StorageDelete>;

// Applies `ops` in order at `timestamp`.
absl::Status ApplyStorageOps(absl::Time timestamp,
                             const std::vector<StorageOp>& ops,
                             Storage* storage);

// Records the Writes and Deletes made through it, as physical effects for the
// commit log. With `forward`, they also reach `base` at once (a schema
// change's backfill reads what it wrote); without, only reads do, and the
// caller applies the recorded ops later. Everything else goes to `base`.
// Not thread-safe.
class RecordingStorage : public Storage {
 public:
  RecordingStorage(Storage* base, bool forward)
      : base_(base), forward_(forward) {}

  const std::vector<StorageOp>& ops() const { return ops_; }

  absl::Status Lookup(absl::Time timestamp, const TableID& table_id,
                      const Key& key, const std::vector<ColumnID>& column_ids,
                      std::vector<googlesql::Value>* values) const override {
    return base_->Lookup(timestamp, table_id, key, column_ids, values);
  }

  absl::Status Read(absl::Time timestamp, const TableID& table_id,
                    const KeyRange& key_range,
                    const std::vector<ColumnID>& column_ids,
                    std::unique_ptr<StorageIterator>* itr) const override {
    return base_->Read(timestamp, table_id, key_range, column_ids, itr);
  }

  absl::Status Write(absl::Time timestamp, const TableID& table_id,
                     const Key& key, const std::vector<ColumnID>& column_ids,
                     const std::vector<googlesql::Value>& values) override;

  absl::Status Delete(absl::Time timestamp, const TableID& table_id,
                      const KeyRange& key_range) override;

  absl::Status MarkWritten(absl::Time timestamp, const TableID& table_id,
                           const Key& key) override {
    return base_->MarkWritten(timestamp, table_id, key);
  }

  bool HasVersionsAfter(absl::Time timestamp, const TableID& table_id,
                        const KeyRange& key_range) const override {
    return base_->HasVersionsAfter(timestamp, table_id, key_range);
  }

  void SetVersionRetentionPeriod(absl::Duration period) override {
    base_->SetVersionRetentionPeriod(period);
  }
  void CleanUpDeletedTables(absl::Time timestamp) override {
    base_->CleanUpDeletedTables(timestamp);
  }
  void CleanUpDeletedColumns(absl::Time timestamp) override {
    base_->CleanUpDeletedColumns(timestamp);
  }
  void MarkDroppedTable(absl::Time timestamp, TableID table_id) override {
    base_->MarkDroppedTable(timestamp, table_id);
  }
  void MarkDroppedColumn(absl::Time timestamp, TableID table_id,
                         ColumnID column_id) override {
    base_->MarkDroppedColumn(timestamp, table_id, column_id);
  }
  void RollBackVersionsAt(absl::Time timestamp) override {
    base_->RollBackVersionsAt(timestamp);
  }

  // Also forgets the ops recorded after the savepoint.
  std::unique_ptr<StorageSavepoint> SaveVersionsAt(
      absl::Time timestamp) override;
  void RestoreVersionsAt(absl::Time timestamp,
                         const StorageSavepoint& savepoint) override;

  void UnmarkDroppedAt(
      absl::Time timestamp, const absl::flat_hash_set<TableID>& live_tables,
      const absl::flat_hash_set<ColumnID>& live_columns) override {
    base_->UnmarkDroppedAt(timestamp, live_tables, live_columns);
  }

 private:
  Storage* const base_;
  const bool forward_;
  std::vector<StorageOp> ops_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_STORAGE_RECORDING_STORAGE_H_
