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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_COMMON_IDS_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_COMMON_IDS_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

template <typename IdType>
class UniqueIdGenerator {
 public:
  UniqueIdGenerator() : next_seq_(0) {}
  explicit UniqueIdGenerator(int64_t starting_seq) : next_seq_(starting_seq) {}

  // Generate the next unique ID. An ID preassigned to `prefix` is returned
  // instead, once.
  IdType NextId(absl::string_view prefix) ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    if (auto it = preassigned_.find(prefix); it != preassigned_.end()) {
      IdType id = std::move(it->second);
      preassigned_.erase(it);
      return id;
    }
    return IdType{absl::StrCat(prefix, ":", next_seq_++)};
  }

  // Makes NextId(prefix) return the given IDs, so that rebuilding a schema
  // from its DDL gives its objects the IDs their stored data uses.
  void Preassign(absl::flat_hash_map<std::string, IdType> ids)
      ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    preassigned_ = std::move(ids);
  }

  // Clears the preassigned IDs NextId did not return, and returns their
  // prefixes.
  std::vector<std::string> TakeUnusedPreassigned() ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    std::vector<std::string> unused;
    for (const auto& [prefix, id] : preassigned_) unused.push_back(prefix);
    preassigned_.clear();
    return unused;
  }

  // The sequence number the next generated ID will use.
  int64_t next_seq() ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    return next_seq_;
  }
  void set_next_seq(int64_t next_seq) ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    next_seq_ = next_seq;
  }

  // Generate the next unique ID.
  IdType NextId() ABSL_LOCKS_EXCLUDED(mu_) {
    absl::MutexLock lock(&mu_);
    return IdType{next_seq_++};
  }

 private:
  absl::Mutex mu_;
  int64_t next_seq_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, IdType> preassigned_ ABSL_GUARDED_BY(mu_);
};

// Unique identifier associated with a table. TableID is guaranteed to be unique
// within a single database.
using TableID = std::string;

// A TableID generator. Each database has a single TableIDGenerator.
using TableIDGenerator = UniqueIdGenerator<TableID>;

// Unique identifier associated with a change stream. ChangeStreamID is
// guaranteed to be unique within a single database.
using ChangeStreamID = std::string;

// A ChangeStreamID generator. Each database has a single
// ChangeStreamIDGenerator.
using ChangeStreamIDGenerator = UniqueIdGenerator<ChangeStreamID>;

// Unique identifier associated with a column. ColumnID is guaranteed to be
// unique within a single table.
using ColumnID = std::string;

// A ColumnID generator. Each table has a single ColumnIDGenerator.
using ColumnIDGenerator = UniqueIdGenerator<ColumnID>;

// Unique identifier associated with a sequence. SequenceID is guaranteed to be
// unique within a single database.
using SequenceID = std::string;

// A SequenceID generator. Each database has a single SequenceIDGenerator.
using SequenceIDGenerator = UniqueIdGenerator<SequenceID>;

// Unique identifier associated with a named schema. NamedSchemaID is guaranteed
// to be unique within a single database.
using NamedSchemaID = std::string;

// A NamedSchemaID generator. Each database has a single NamedSchemaIDGenerator.
using NamedSchemaIDGenerator = UniqueIdGenerator<NamedSchemaID>;

// Unique identifier associated with a transaction.
using TransactionID = int64_t;

// A TransactionID generator. Each database has a single TransactionIDGenerator.
using TransactionIDGenerator = UniqueIdGenerator<TransactionID>;

// The priority associated with a transaction.
using TransactionPriority = int64_t;

// A sentinel transaction ID which will never be assigned to valid transactions.
constexpr TransactionID kInvalidTransactionID = -1;

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_COMMON_IDS_H_
