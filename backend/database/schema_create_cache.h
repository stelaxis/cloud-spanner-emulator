//
// Copyright 2026 Google LLC
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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_SCHEMA_CREATE_CACHE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_SCHEMA_CREATE_CACHE_H_

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "googlesql/public/types/type_factory.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/updater/schema_updater.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Returns a deep copy of `schema` that shares no object with it: every schema
// node is cloned, types that a type factory owns are rebuilt in
// `type_factory`, and the proto bundle is rebuilt from its descriptors.
// Sequences get new IDs, so the copy's sequences start where a new sequence
// starts, and change streams are created at `now`. Table and column IDs are
// kept.
//
// Fails with UNIMPLEMENTED for schemas it cannot copy: a function whose
// signature has a type that a type factory owns, or default arguments.
absl::StatusOr<std::unique_ptr<const Schema>> CopySchema(
    const Schema& schema, googlesql::TypeFactory* type_factory,
    std::string_view database_id, absl::Time now);

// Remembers the schemas that CreateDatabase built from DDL, so that a later
// CreateDatabase with the same request copies the schema (CopySchema) instead
// of processing the DDL again, which takes time quadratic in the number of
// statements.
//
// A request's key is everything the DDL's outcome depends on: the statements,
// the dialect, the proto descriptors and the emulator feature flags. The
// database's name only names the schema; command-line flags are fixed for the
// life of the process, which is the life of the cache. Only schemas of
// successful creates are added, so a failing request always processes its DDL
// and fails as it would without the cache.
//
// The cache holds at most `capacity` schemas and evicts the least recently
// used. It is thread-safe.
class SchemaCreateCache {
 public:
  // A cached schema, and the state of a database's ID generators right after
  // it was created with it.
  struct Entry {
    // Owns the types of `schema`, so it is declared first.
    std::unique_ptr<googlesql::TypeFactory> type_factory;
    // Null if the schema cannot be copied (CopySchema); then creates with the
    // same key process their DDL.
    std::unique_ptr<const Schema> schema;
    int64_t next_table_id = 0;
    int64_t next_column_id = 0;
    std::optional<uint32_t> next_postgresql_oid;
  };

  explicit SchemaCreateCache(int capacity) : capacity_(capacity) {}

  // The process-wide cache, holding --schema_create_cache_size schemas; null
  // if that is 0.
  static SchemaCreateCache* Global();

  // For tests: makes Global() return `cache`, which may be null, and returns
  // the cache it returned before. Not thread-safe with Global().
  static SchemaCreateCache* SetGlobalForTesting(SchemaCreateCache* cache);

  static std::string Key(const SchemaChangeOperation& operation);

  // Returns the entry for `key`, or null.
  std::shared_ptr<const Entry> Lookup(const std::string& key)
      ABSL_LOCKS_EXCLUDED(mu_);

  // As Lookup, without counting a hit or miss or marking the entry used.
  std::shared_ptr<const Entry> Peek(const std::string& key) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Adds or replaces the entry for `key`.
  void Insert(const std::string& key, std::shared_ptr<const Entry> entry)
      ABSL_LOCKS_EXCLUDED(mu_);

  int64_t hits() const ABSL_LOCKS_EXCLUDED(mu_);
  int64_t misses() const ABSL_LOCKS_EXCLUDED(mu_);
  int size() const ABSL_LOCKS_EXCLUDED(mu_);

 private:
  using Lru = std::list<std::pair<std::string, std::shared_ptr<const Entry>>>;

  const int capacity_;
  mutable absl::Mutex mu_;
  // Most recently used first.
  Lru lru_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, Lru::iterator> index_ ABSL_GUARDED_BY(mu_);
  int64_t hits_ ABSL_GUARDED_BY(mu_) = 0;
  int64_t misses_ ABSL_GUARDED_BY(mu_) = 0;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_SCHEMA_CREATE_CACHE_H_
