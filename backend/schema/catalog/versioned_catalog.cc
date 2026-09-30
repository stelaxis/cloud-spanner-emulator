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

#include "backend/schema/catalog/versioned_catalog.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/common/utils.h"
#include "backend/schema/catalog/schema.h"
#include "googlesql/base/ret_check.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

absl::StatusOr<absl::Duration> ParseVersionRetentionPeriod(
    absl::string_view version_retention_period) {
  int64_t retention_period = ParseSchemaTimeSpec(version_retention_period);

  if (retention_period <= 0) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid retention period: ", version_retention_period));
  } else if (retention_period > (7 * 24 * 60 * 60)) {
    return absl::InvalidArgumentError(
        absl::StrCat("Retention period must be less than 7 days: ",
                     version_retention_period));
  } else {
    if (retention_period < 60 * 60) {
      ABSL_LOG(WARNING) << "Retention period has been set to less than 1 hour. "
                   << "This is only supported in the emulator. "
                   << "This is not supported by Spanner.";
    }
    return absl::Seconds(retention_period);
  }
}

}  // namespace

VersionedCatalog::VersionedCatalog() {
  schemas_[absl::InfinitePast()] = std::make_unique<const Schema>();
}

VersionedCatalog::VersionedCatalog(
    std::unique_ptr<const Schema> initial_schema) {
  schemas_[absl::InfinitePast()] = std::move(initial_schema);
}

VersionedCatalog::VersionedCatalog(std::unique_ptr<const Schema> initial_schema,
                                   absl::Duration version_retention_period)
    : VersionedCatalog(std::move(initial_schema)) {
  version_retention_period_ = version_retention_period;
}

const Schema* VersionedCatalog::GetSchema(absl::Time timestamp) const {
  absl::MutexLock lock(mu_);
  auto itr = schemas_.upper_bound(timestamp);
  itr--;
  return itr->second.get();
}

const Schema* VersionedCatalog::GetLatestSchema() const {
  return GetSchema(absl::InfiniteFuture());
}

std::shared_ptr<const Schema> VersionedCatalog::GetSchemaShared(
    absl::Time timestamp) const {
  absl::MutexLock lock(mu_);
  auto itr = schemas_.upper_bound(timestamp);
  itr--;
  return itr->second;
}

std::shared_ptr<const Schema> VersionedCatalog::GetLatestSchemaShared() const {
  return GetSchemaShared(absl::InfiniteFuture());
}

std::shared_ptr<const Schema> VersionedCatalog::GetSchemaShared(
    absl::Time timestamp, bool* swept) const {
  absl::MutexLock lock(mu_);
  auto swept_itr = swept_.upper_bound(timestamp);
  *swept =
      swept_itr != swept_.begin() && timestamp < std::prev(swept_itr)->second;
  auto itr = schemas_.upper_bound(timestamp);
  itr--;
  return itr->second;
}

absl::Status VersionedCatalog::AddSchema(absl::Time creation_time,
                                         std::unique_ptr<const Schema> schema) {
  absl::MutexLock lock(mu_);
  GOOGLESQL_RET_CHECK(creation_time > schemas_.rbegin()->first)
      << "Failed to insert schema at "
      << absl::FormatTime(creation_time, absl::UTCTimeZone())
      << ": the latest schema creation timestamp is "
      << absl::FormatTime(schemas_.rbegin()->first, absl::UTCTimeZone());
  auto version_retention_period =
      ParseVersionRetentionPeriod(schema->version_retention_period());
  if (!version_retention_period.ok()) {
    return version_retention_period.status();
  }
  version_retention_period_ = *version_retention_period;
  schemas_[creation_time] = std::move(schema);
  return absl::OkStatus();
}

int VersionedCatalog::RemoveExpiredSchemas(absl::Time timestamp, bool swept) {
  // Released after mu_: destroying a schema takes a while.
  std::vector<std::shared_ptr<const Schema>> removed;
  absl::MutexLock lock(mu_);
  auto upper_bound =
      schemas_.upper_bound(timestamp - version_retention_period_);
  auto it = ++schemas_.begin();  // Skip the infinite past schema.
  const absl::Time oldest_removed =
      it == schemas_.end() ? absl::InfinitePast() : it->first;
  while (it != upper_bound) {
    auto next = it;
    if (++next == upper_bound) {
      // The current schema needs to be kept to cover the retention period.
      break;
    }
    removed.push_back(std::move(it->second));
    it = schemas_.erase(it);
  }
  if (swept && !removed.empty()) {
    // `it` is the schema kept after the removed ones.
    auto previous = swept_.empty() ? swept_.end() : std::prev(swept_.end());
    if (previous != swept_.end() && previous->second == oldest_removed) {
      previous->second = it->first;
    } else {
      swept_[oldest_removed] = it->first;
    }
  }
  return removed.size();
}

std::vector<absl::Time> VersionedCatalog::SchemaTimestampsForTesting() const {
  absl::MutexLock lock(mu_);
  std::vector<absl::Time> timestamps;
  for (const auto& [timestamp, schema] : schemas_) {
    timestamps.push_back(timestamp);
  }
  return timestamps;
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
