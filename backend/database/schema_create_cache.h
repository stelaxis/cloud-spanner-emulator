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
#include <functional>
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
// Sequences get new IDs starting with `sequence_id_prefix`, so the copy's
// sequences start where a new sequence starts, and their counters are released
// with the prefix (Sequence::RemoveSequenceCountersWithIdPrefix). Change
// streams are created at `now`, or keep their creation time if it is unset.
// Table and column IDs are kept.
//
// Fails with UNIMPLEMENTED for schemas it cannot copy: a function whose
// signature has a type that a type factory owns, or default arguments.
absl::StatusOr<std::unique_ptr<const Schema>> CopySchema(
    const Schema& schema, googlesql::TypeFactory* type_factory,
    std::string_view database_id, std::string_view sequence_id_prefix,
    std::optional<absl::Time> now);

// Remembers the schemas that CreateDatabase built from DDL, so that a later
// CreateDatabase with the same request copies the schema (CopySchema) instead
// of processing the DDL again, which takes time quadratic in the number of
// statements.
//
// A request's key is everything the DDL's outcome may depend on: the
// statements, the dialect, the proto descriptors, and the settings (see
// Settings). The database's name only names the schema. Only schemas of
// successful creates are added, so a failing request always processes its DDL
// and fails as it would without the cache. A schema is added only if the
// settings did not change while it was created (SettingsUnchanged).
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

  // The settings DDL processing may read: the emulator feature flags and the
  // value of every command-line flag linked into the process, except the few
  // that the emulator writes at runtime and DDL processing never reads (the
  // change stream churner's intervals, and this cache's size).
  struct Settings {
    // EmulatorFeatureFlags::Snapshot::generation, read with the flags.
    int64_t generation = 0;
    std::string values;
  };
  static Settings CurrentSettings();

  // True if no setting changed since `before` was taken. Any write to the
  // feature flags since then is caught, even one undone before now, since it
  // moves their generation. A command-line flag is caught only if its value
  // differs now: outside tests, nothing writes the flags in the settings
  // after startup.
  static bool SettingsUnchanged(const Settings& before);

  static std::string Key(const SchemaChangeOperation& operation,
                         const Settings& settings);

  // Returns the entry for `key`, or null.
  std::shared_ptr<const Entry> Lookup(const std::string& key)
      ABSL_LOCKS_EXCLUDED(mu_);

  // As Lookup, without counting a hit or miss or marking the entry used.
  std::shared_ptr<const Entry> Peek(const std::string& key) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Adds or replaces the entry for `key`.
  void Insert(const std::string& key, std::shared_ptr<const Entry> entry)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Outcomes of creates, for tests and logs.
  void RecordCopy() ABSL_LOCKS_EXCLUDED(mu_);
  void RecordFallback() ABSL_LOCKS_EXCLUDED(mu_);
  void RecordRejected() ABSL_LOCKS_EXCLUDED(mu_);

  int64_t hits() const ABSL_LOCKS_EXCLUDED(mu_);
  int64_t misses() const ABSL_LOCKS_EXCLUDED(mu_);
  // Creates completed from a copy of an entry's schema.
  int64_t copies() const ABSL_LOCKS_EXCLUDED(mu_);
  // Creates that found an entry with a schema but could not copy it, and so
  // processed their DDL.
  int64_t fallbacks() const ABSL_LOCKS_EXCLUDED(mu_);
  // Schemas not added because the settings changed while they were created.
  int64_t rejected() const ABSL_LOCKS_EXCLUDED(mu_);
  int size() const ABSL_LOCKS_EXCLUDED(mu_);

  // For tests: Database::Create runs `after_key` once it has taken the
  // settings and looked up its key, and `before_publish` just before it
  // checks the settings to add a schema it created. Set before concurrent use.
  static void SetCreateHooksForTesting(std::function<void()> after_key,
                                       std::function<void()> before_publish) {
    after_key_hook_ = std::move(after_key);
    before_publish_hook_ = std::move(before_publish);
  }
  static void RunAfterKeyHook() {
    if (after_key_hook_ != nullptr) after_key_hook_();
  }
  static void RunBeforePublishHook() {
    if (before_publish_hook_ != nullptr) before_publish_hook_();
  }

 private:
  using Lru = std::list<std::pair<std::string, std::shared_ptr<const Entry>>>;

  const int capacity_;
  mutable absl::Mutex mu_;
  // Most recently used first.
  Lru lru_ ABSL_GUARDED_BY(mu_);
  absl::flat_hash_map<std::string, Lru::iterator> index_ ABSL_GUARDED_BY(mu_);
  int64_t hits_ ABSL_GUARDED_BY(mu_) = 0;
  int64_t misses_ ABSL_GUARDED_BY(mu_) = 0;
  int64_t copies_ ABSL_GUARDED_BY(mu_) = 0;
  int64_t fallbacks_ ABSL_GUARDED_BY(mu_) = 0;
  int64_t rejected_ ABSL_GUARDED_BY(mu_) = 0;

  inline static std::function<void()> after_key_hook_;
  inline static std::function<void()> before_publish_hook_;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_DATABASE_SCHEMA_CREATE_CACHE_H_
