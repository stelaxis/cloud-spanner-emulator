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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_LOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_LOG_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/schema.h"
#include "backend/storage/recording_storage.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// A schema and the ID allocator positions that go with it.
struct PersistedSchema {
  std::shared_ptr<const Schema> schema;
  int64_t next_table_seq = 0;
  int64_t next_column_seq = 0;
};

// Makes one database's commits and schema changes durable (--data_dir).
//
// The commit gate is shared by every database of the emulator. A commit or
// schema change holds it from reserving its timestamp until its effects are
// installed, so the log holds records in commit-timestamp order across all
// databases. Log* returns only once the record is synced; a commit whose
// record fails must not take effect, and the log then refuses every later
// record.
class DatabaseLog {
 public:
  virtual ~DatabaseLog() = default;

  virtual absl::Mutex* commit_gate() = 0;

  // OK unless a record has failed, after which every Log* call fails.
  virtual absl::Status health() = 0;

  // Logs a commit's resolved physical writes. Called under the gate.
  virtual absl::Status LogCommit(absl::Time commit_timestamp,
                                 const std::vector<StorageOp>& ops) = 0;

  // Logs the schema resulting from a schema change and the writes of its
  // backfills. Called under the gate, after the change is applied in memory
  // (backfills read their own writes) but before it is visible.
  virtual absl::Status LogSchemaChange(absl::Time commit_timestamp,
                                       const PersistedSchema& schema,
                                       const std::vector<StorageOp>& ops) = 0;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_DATABASE_LOG_H_
