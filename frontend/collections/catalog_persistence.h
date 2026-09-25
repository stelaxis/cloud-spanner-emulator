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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COLLECTIONS_CATALOG_PERSISTENCE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COLLECTIONS_CATALOG_PERSISTENCE_H_

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "backend/database/database_log.h"
#include "frontend/entities/database.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Makes instance and database creation and deletion durable (--data_dir).
// The managers call the Log* methods under their own lock, after their checks
// pass and before the change becomes visible; a failure leaves it undone.
class CatalogPersistence {
 public:
  virtual ~CatalogPersistence() = default;

  // The log for a database about to be created.
  virtual std::unique_ptr<backend::DatabaseLog> NewDatabaseLog() = 0;

  virtual absl::Status LogCreateInstance(
      const std::string& instance_uri,
      const google::spanner::admin::instance::v1::Instance& instance) = 0;
  virtual absl::Status LogDeleteInstance(const std::string& instance_uri) = 0;

  // `database` was created with a log from NewDatabaseLog.
  virtual absl::Status LogCreateDatabase(
      const std::string& instance_uri,
      const std::shared_ptr<Database>& database, absl::Time create_time) = 0;
  virtual absl::Status LogDropDatabase(const Database& database) = 0;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COLLECTIONS_CATALOG_PERSISTENCE_H_
