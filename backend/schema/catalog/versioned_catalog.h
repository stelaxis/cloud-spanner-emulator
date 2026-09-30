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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/schema/catalog/schema.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// VersionedCatalog owns schemas that belongs to a single database, and keep a
// mapping between the schema creation time and the schema object.
class VersionedCatalog {
 public:
  // The default constructor creates an empty schema in the catalog and assigns
  // absl::InfinitePast() as its creation timestamp.
  VersionedCatalog();

  // The single-argument constructor is used when a database is created with an
  // initial schema specified. The initial_schema is added to the catalog and
  // absl::InfinitePast() is assigned as its creation timestamp.
  explicit VersionedCatalog(std::unique_ptr<const Schema> initial_schema);

  // As above, with the given version retention period.
  VersionedCatalog(std::unique_ptr<const Schema> initial_schema,
                   absl::Duration version_retention_period);

  // Finds the newest schema that is created at or before a given timestamp and
  // returns a pointer to that schema object. There is always a first schema in
  // each VersionedCatalog, which has a creation timestamp of
  // absl::InfinitePast() (see comments of the constructors above). Therefore,
  // GetSchema never returns a nullptr.
  const Schema* GetSchema(absl::Time timestamp) const ABSL_LOCKS_EXCLUDED(mu_);

  // Returns the latest schema object in the catalog. Will return the first
  // schema initialized if there are no subsequent new schema. Therefore,
  // GetLatestSchema never returns a nullptr.
  const Schema* GetLatestSchema() const ABSL_LOCKS_EXCLUDED(mu_);

  // GetSchema and GetLatestSchema with shared ownership, so that the schema
  // outlives its removal from the catalog (RemoveExpiredSchemas).
  std::shared_ptr<const Schema> GetSchemaShared(absl::Time timestamp) const
      ABSL_LOCKS_EXCLUDED(mu_);
  std::shared_ptr<const Schema> GetLatestSchemaShared() const
      ABSL_LOCKS_EXCLUDED(mu_);

  // GetSchemaShared, also setting `*swept` if the schema in effect at
  // `timestamp` was removed by a sweep (RemoveExpiredSchemas with `swept`), or
  // by a schema change between two sweeps that removed schemas: the schema
  // returned is then an older one.
  std::shared_ptr<const Schema> GetSchemaShared(absl::Time timestamp,
                                                bool* swept) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Adds a schema at a given timestamp. Returns an error if creation_time is
  // the same or prior to the largest timestamp in all of the schemas. In this
  // case, the new schema will not be added.
  absl::Status AddSchema(absl::Time creation_time,
                         std::unique_ptr<const Schema> schema)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Removes the schemas that no read at or after `timestamp` minus the
  // retention period needs: all but the first schema, the newest one created
  // at or before that time and the ones after it. Returns how many it removed.
  // A removed schema is destroyed once nothing else holds it, never under the
  // catalog's lock. With `swept`, the removal is a sweep's, and lookups at the
  // times the removed schemas were in effect report it from then on, as do
  // lookups at the times of the schemas a schema change removed since an
  // earlier sweep removed any.
  int RemoveExpiredSchemas(absl::Time timestamp, bool swept = false)
      ABSL_LOCKS_EXCLUDED(mu_);

  // The creation times of the schemas in the catalog, oldest first. For tests.
  std::vector<absl::Time> SchemaTimestampsForTesting() const
      ABSL_LOCKS_EXCLUDED(mu_);

  // The ranges of time at which lookups report a sweep: none until a sweep
  // removes a schema, then one. For tests.
  std::vector<std::pair<absl::Time, absl::Time>> SweptRangesForTesting() const
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Duration version_retention_period() const {
    absl::MutexLock lock(&mu_);
    return version_retention_period_;
  }

 private:
  // For guarding concurrent access to `schemas_`.
  mutable absl::Mutex mu_;

  // A mapping between the creation time of each schema in the catalog and the
  // schema objects. Used to look up schema at a specific point in time. A first
  // schema is always created in the constructors. Therefore, this map is never
  // empty.
  //
  // Note that this cannot be changed into a hash map (e.g. std::unordered_map)
  // because the lookup of schemas by creation timestamp depends on the ordering
  // of keys in this map.
  std::map<absl::Time, std::shared_ptr<const Schema>> schemas_
      ABSL_GUARDED_BY(mu_);

  // The retention period for schema versions.
  absl::Duration version_retention_period_ ABSL_GUARDED_BY(mu_) =
      absl::Hours(1);

  // Lookups at [swept_begin_, swept_end_) report a sweep. The range runs from
  // the creation of the oldest schema that the first sweep to remove any
  // removed, to the creation of the schema that the latest such sweep kept.
  // Schemas are removed oldest first, so no schema in effect in it is left:
  // it also covers those a schema change removed between two sweeps, where a
  // lookup could only return an older schema. That keeps it one range however
  // often sweeps and schema changes alternate. Empty until a sweep removes a
  // schema.
  absl::Time swept_begin_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();
  absl::Time swept_end_ ABSL_GUARDED_BY(mu_) = absl::InfinitePast();
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_VERSIONED_CATALOG_H_
