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

#include "backend/storage/in_memory_storage.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "googlesql/public/value.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "backend/storage/in_memory_iterator.h"
#include "common/errors.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

static constexpr char kExistsColumn[] = "_exists";

}  // namespace

void InMemoryStorage::RemoveExpiredVersions(Cell& cell, absl::Time timestamp) {
  absl::MutexLock lock(version_retention_period_mu_);
  auto it = cell.begin();
  auto upper_bound = cell.upper_bound(timestamp - version_retention_period_);
  while (it != upper_bound) {
    auto next = it;
    if (++next == upper_bound) {
      // The current value needs to be kept to cover the retention period.
      break;
    }
    it = cell.erase(it);
  }
}

googlesql::Value InMemoryStorage::GetCellValueAtTimestamp(
    const Row& row, const ColumnID& column_id, absl::Time timestamp) const {
  // Perform the lookup for given cell.
  auto cell_itr = row.find(column_id);
  if (cell_itr == row.end()) {
    return googlesql::Value();
  }
  const Cell& cell = cell_itr->second;
  auto val_itr = cell.upper_bound(timestamp);

  // Timestamp is earlier than the time the cell was first written to.
  if (val_itr == cell.begin()) {
    return googlesql::Value();
  }

  // Fetch the value from the column.
  --val_itr;
  return val_itr->second;
}

bool InMemoryStorage::Exists(const Row& row, absl::Time timestamp) const {
  googlesql::Value value =
      GetCellValueAtTimestamp(row, kExistsColumn, timestamp);
  return value.is_valid() && value.bool_value();
}

absl::Status InMemoryStorage::Lookup(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    std::vector<googlesql::Value>* values) const {
  absl::MutexLock lock(mu_);

  // Validate the request.
  if (!column_ids.empty() && values == nullptr) {
    return error::Internal(
        "InMemoryStorage::Lookup was passed a nullptr for "
        "values, but had non-empty column_ids.");
  }
  if (values != nullptr) {
    values->clear();
  }

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }
  const Table& table = table_itr->second;

  // Lookup for given key.
  auto row_itr = table.find(key);
  if (row_itr == table.end()) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("Key: ", key.DebugString(), " not found for table: ",
                     table_id, " at timestamp: ", absl::FormatTime(timestamp)));
  }
  const Row& row = row_itr->second;

  // Verify if the row exists at the given timestamp.
  if (!Exists(row, timestamp)) {
    return absl::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat(
            "Key: ", key.DebugString(), " does not exist for table: ", table_id,
            " at the given timestamp: " + absl::FormatTime(timestamp)));
  }

  // For request without columns, return ok since the key exist.
  if (column_ids.empty()) {
    return absl::OkStatus();
  }

  // Fetch the value from the cell at the given timestamp.
  for (int i = 0; i < column_ids.size(); ++i) {
    values->emplace_back(
        GetCellValueAtTimestamp(row, column_ids[i], timestamp));
  }

  return absl::OkStatus();
}

absl::Status InMemoryStorage::Read(
    absl::Time timestamp, const TableID& table_id, const KeyRange& key_range,
    const std::vector<ColumnID>& column_ids,
    std::unique_ptr<StorageIterator>* itr) const {
  absl::MutexLock lock(mu_);

  // Validate the request.
  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("InMemoryStorage::Read should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }

  std::vector<FixedRowStorageIterator::Row> rows;
  // Return an empty iterator for empty key_range.
  if (key_range.start_key() >= key_range.limit_key()) {
    *itr = std::make_unique<FixedRowStorageIterator>();
    return absl::OkStatus();
  }

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    *itr = std::make_unique<FixedRowStorageIterator>();
    return absl::OkStatus();
  }
  const Table& table = table_itr->second;

  // Lookup keys from the given key range.
  auto row_start_itr = table.lower_bound(key_range.start_key());
  auto row_end_itr = table.lower_bound(key_range.limit_key());
  for (auto itr = row_start_itr; itr != row_end_itr; ++itr) {
    const InMemoryStorage::Row& row = itr->second;
    if (!Exists(row, timestamp)) {
      continue;
    }

    std::vector<googlesql::Value> values;
    values.reserve(column_ids.size());
    for (const ColumnID& column_id : column_ids) {
      values.emplace_back(GetCellValueAtTimestamp(row, column_id, timestamp));
    }
    rows.emplace_back(std::make_pair(itr->first, values));
  }
  *itr = std::make_unique<FixedRowStorageIterator>(std::move(rows));
  return absl::OkStatus();
}

absl::Status InMemoryStorage::Write(
    absl::Time timestamp, const TableID& table_id, const Key& key,
    const std::vector<ColumnID>& column_ids,
    const std::vector<googlesql::Value>& values) {
  absl::MutexLock lock(mu_);

  // Add the table if it does not exist.
  Table& table = tables_[table_id];

  // Add the row with _exists system column if it does not exist.
  Row& row = table[key];
  // A write of no columns to an existing row must still leave a version.
  if (!Exists(row, timestamp) || column_ids.empty()) {
    Cell& cell = row[kExistsColumn];
    cell[timestamp] = googlesql::values::Bool(true);
    RemoveExpiredVersions(cell, timestamp);
  }

  // Add the values for the given columns.
  for (int i = 0; i < column_ids.size(); ++i) {
    Cell& cell = row[column_ids[i]];
    cell[timestamp] = values[i];
    RemoveExpiredVersions(cell, timestamp);
  }
  NoteVersion(table_id, timestamp);

  return absl::OkStatus();
}

void InMemoryStorage::NoteVersion(const TableID& table_id,
                                  absl::Time timestamp) {
  auto [it, inserted] = latest_version_.try_emplace(table_id, timestamp);
  if (!inserted && it->second < timestamp) {
    it->second = timestamp;
  }
}

absl::Status InMemoryStorage::MarkWritten(absl::Time timestamp,
                                          const TableID& table_id,
                                          const Key& key) {
  absl::MutexLock lock(mu_);
  Row& row = tables_[table_id][key];
  Cell& cell = row[kExistsColumn];
  cell[timestamp] = googlesql::values::Bool(Exists(row, timestamp));
  RemoveExpiredVersions(cell, timestamp);
  NoteVersion(table_id, timestamp);
  return absl::OkStatus();
}

bool InMemoryStorage::HasVersionsAfter(absl::Time timestamp,
                                       const TableID& table_id,
                                       const KeyRange& key_range) const {
  absl::MutexLock lock(mu_);
  auto latest_itr = latest_version_.find(table_id);
  if (latest_itr == latest_version_.end() || latest_itr->second <= timestamp) {
    return false;
  }
  if (key_range.start_key() >= key_range.limit_key()) {
    return false;
  }
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    return false;
  }
  const Table& table = table_itr->second;
  auto row_end_itr = table.lower_bound(key_range.limit_key());
  for (auto itr = table.lower_bound(key_range.start_key()); itr != row_end_itr;
       ++itr) {
    for (const auto& [column_id, cell] : itr->second) {
      // The newest version of a cell is never garbage collected.
      if (!cell.empty() && cell.rbegin()->first > timestamp) {
        return true;
      }
    }
  }
  return false;
}

absl::Status InMemoryStorage::Delete(absl::Time timestamp,
                                     const TableID& table_id,
                                     const KeyRange& key_range) {
  absl::MutexLock lock(mu_);

  if (!key_range.IsClosedOpen()) {
    return error::Internal(
        absl::StrCat("InMemoryStorage::Delete should be called "
                     "with ClosedOpen key range, found: ",
                     key_range.DebugString()));
  }
  if (key_range.start_key() >= key_range.limit_key()) {
    return absl::OkStatus();
  }

  // Lookup for given table.
  auto table_itr = tables_.find(table_id);
  if (table_itr == tables_.end()) {
    return absl::OkStatus();
  }
  Table& table = table_itr->second;

  // Lookup keys from the given key range.
  auto row_start_itr = table.lower_bound(key_range.start_key());
  if (row_start_itr == table.end()) {
    return absl::OkStatus();
  }
  auto row_end_itr = table.lower_bound(key_range.limit_key());

  // Mark the keys as deleted.
  for (auto itr = row_start_itr; itr != row_end_itr; ++itr) {
    if (!Exists(itr->second, timestamp)) {
      continue;
    }
    NoteVersion(table_id, timestamp);

    for (const auto& columns : itr->second) {
      if (columns.first == kExistsColumn) {
        Cell& cell = itr->second[kExistsColumn];
        cell[timestamp] = googlesql::values::Bool(false);
        RemoveExpiredVersions(cell, timestamp);
      } else {
        // Column values are marked invalid googlesql::Value to avoid reading
        // the value of the cell before the delete.
        Cell& cell = itr->second[columns.first];
        cell[timestamp] = googlesql::Value();
        RemoveExpiredVersions(cell, timestamp);
      }
    }
  }
  return absl::OkStatus();
}

void InMemoryStorage::SetVersionRetentionPeriod(
    const absl::Duration version_retention_period) {
  absl::MutexLock lock(version_retention_period_mu_);
  version_retention_period_ = version_retention_period;
}

void InMemoryStorage::CleanUpDeletedTables(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  absl::MutexLock version_retention_period_lock(version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  // Remove expired dropped tables.
  for (auto it = dropped_tables_.begin();
       it != dropped_tables_.upper_bound(expiration_time);) {
    tables_.erase(it->second);
    it = dropped_tables_.erase(it);
  }
}

void InMemoryStorage::CleanUpDeletedColumns(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  absl::MutexLock version_retention_period_lock(version_retention_period_mu_);
  absl::Time expiration_time = timestamp - version_retention_period_;

  // Remove expired dropped columns.
  for (auto it = dropped_columns_.begin();
       it != dropped_columns_.upper_bound(expiration_time);) {
    auto [table_id, column_id] = it->second;
    for (auto& [_, row] : tables_[table_id]) {
      row.erase(column_id);
    }
    it = dropped_columns_.erase(it);
  }
}

std::vector<TableID> InMemoryStorage::TableIdsForTesting() const {
  absl::MutexLock lock(mu_);
  std::vector<TableID> ids;
  for (const auto& [table_id, table] : tables_) ids.push_back(table_id);
  std::sort(ids.begin(), ids.end());
  return ids;
}

void InMemoryStorage::RemoveVersionsAtLocked(absl::Time timestamp) {
  std::vector<TableID> emptied_tables;
  for (auto table = tables_.begin(); table != tables_.end(); ++table) {
    bool touched = false;
    absl::Time latest = absl::InfinitePast();
    for (auto row = table->second.begin(); row != table->second.end();) {
      std::vector<ColumnID> emptied;
      for (auto& [column_id, cell] : row->second) {
        touched |= cell.erase(timestamp) > 0;
        if (cell.empty()) {
          emptied.push_back(column_id);
        } else {
          latest = std::max(latest, cell.rbegin()->first);
        }
      }
      for (const ColumnID& column_id : emptied) row->second.erase(column_id);
      row = row->second.empty() ? table->second.erase(row) : std::next(row);
    }
    if (touched) {
      if (latest == absl::InfinitePast()) {
        latest_version_.erase(table->first);
      } else {
        latest_version_[table->first] = latest;
      }
    }
    if (table->second.empty()) emptied_tables.push_back(table->first);
  }
  for (const TableID& table_id : emptied_tables) tables_.erase(table_id);
}

void InMemoryStorage::RollBackVersionsAt(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  RemoveVersionsAtLocked(timestamp);
  dropped_tables_.erase(timestamp);
  dropped_columns_.erase(timestamp);
}

namespace {

struct InMemorySavepoint : public StorageSavepoint {
  struct Version {
    TableID table_id;
    Key key;
    ColumnID column_id;
    googlesql::Value value;
  };
  std::vector<Version> versions;
};

}  // namespace

std::unique_ptr<StorageSavepoint> InMemoryStorage::SaveVersionsAt(
    absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  auto savepoint = std::make_unique<InMemorySavepoint>();
  for (const auto& [table_id, table] : tables_) {
    for (const auto& [key, row] : table) {
      for (const auto& [column_id, cell] : row) {
        if (auto it = cell.find(timestamp); it != cell.end()) {
          savepoint->versions.push_back({table_id, key, column_id, it->second});
        }
      }
    }
  }
  return savepoint;
}

void InMemoryStorage::RestoreVersionsAt(absl::Time timestamp,
                                        const StorageSavepoint& savepoint) {
  absl::MutexLock lock(mu_);
  RemoveVersionsAtLocked(timestamp);
  for (const auto& version :
       static_cast<const InMemorySavepoint&>(savepoint).versions) {
    tables_[version.table_id][version.key][version.column_id][timestamp] =
        version.value;
    NoteVersion(version.table_id, timestamp);
  }
}

void InMemoryStorage::UnmarkDroppedAt(
    absl::Time timestamp, const absl::flat_hash_set<TableID>& live_tables,
    const absl::flat_hash_set<ColumnID>& live_columns) {
  absl::MutexLock lock(mu_);
  if (auto it = dropped_tables_.find(timestamp);
      it != dropped_tables_.end() && live_tables.contains(it->second)) {
    dropped_tables_.erase(it);
  }
  if (auto it = dropped_columns_.find(timestamp);
      it != dropped_columns_.end() &&
      live_columns.contains(it->second.second)) {
    dropped_columns_.erase(it);
  }
}

void InMemoryStorage::MarkDroppedTable(absl::Time timestamp,
                                       TableID dropped_table_id) {
  absl::MutexLock lock(mu_);
  dropped_tables_[timestamp] = dropped_table_id;
}

void InMemoryStorage::MarkDroppedColumn(absl::Time timestamp,
                                        TableID dropped_table_id,
                                        ColumnID dropped_column_id) {
  absl::MutexLock lock(mu_);
  dropped_columns_[timestamp] =
      std::make_pair(dropped_table_id, dropped_column_id);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
