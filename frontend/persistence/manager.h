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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MANAGER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>  // NOLINT

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/database/database_log.h"
#include "common/clock.h"
#include "frontend/collections/catalog_persistence.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/instance_manager.h"
#include "frontend/entities/database.h"
#include "frontend/persistence/file_system.h"
#include "frontend/persistence/log.h"
#include "frontend/persistence/persistence.pb.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

struct PersistenceOptions {
  FileSystem* fs = nullptr;
  std::string dir;
  // Log growth after which a background checkpoint is written.
  int64_t checkpoint_bytes = 64 << 20;
  // How far ahead of the timestamps handed out the durable clock lease is
  // extended. After a crash, the clock restarts at most this far ahead of the
  // time of the crash.
  absl::Duration lease_window = absl::Milliseconds(250);
  // Whether a thread writes checkpoints (on log growth and on SIGUSR1).
  bool background_checkpoints = true;
};

// Keeps the emulator's state in a data directory (--data_dir): one log of
// every durable change, in commit-timestamp order across all databases, plus
// a checkpoint. Open recovers instances, databases, schemas, rows, sequence
// reservations and the clock into the given managers before the server
// serves anything.
//
// The Lean persistence model (verification/lean/TxnSpec/Persistence*.lean)
// is the specification; see docs/persistence.md for what is kept.
class PersistenceManager : public CatalogPersistence {
 public:
  static absl::StatusOr<std::unique_ptr<PersistenceManager>> Open(
      const PersistenceOptions& options, Clock* clock,
      InstanceManager* instance_manager, DatabaseManager* database_manager);

  // Stops the checkpoint thread and detaches from the clock and sequences.
  ~PersistenceManager() override;

  // Writes a checkpoint and removes the log it covers.
  absl::Status Checkpoint() ABSL_LOCKS_EXCLUDED(checkpoint_mu_, gate_);

  // CatalogPersistence.
  std::unique_ptr<backend::DatabaseLog> NewDatabaseLog() override;
  absl::Status LogCreateInstance(
      const std::string& instance_uri,
      const google::spanner::admin::instance::v1::Instance& instance) override;
  absl::Status LogDeleteInstance(const std::string& instance_uri) override;
  absl::Status LogCreateDatabase(const std::string& instance_uri,
                                 const std::shared_ptr<Database>& database,
                                 absl::Time create_time) override;
  absl::Status LogDropDatabase(const Database& database) override;

  absl::Mutex* gate() ABSL_LOCK_RETURNED(gate_) { return &gate_; }

  // Appends and syncs a record.
  absl::Status Append(const Record& record);

  // OK unless a record has failed.
  absl::Status health() { return log_->health(); }

  // The first timestamp at which recovered databases can be read.
  absl::Time restart_floor() const { return restart_floor_; }

 private:
  struct LiveDatabase {
    DatabaseState state;  // without the schema
    std::weak_ptr<Database> database;
  };

  PersistenceManager(const PersistenceOptions& options, Clock* clock)
      : options_(options), clock_(clock) {}

  absl::Status Recover(const LogContents& contents,
                       InstanceManager* instance_manager,
                       DatabaseManager* database_manager);

  void CheckpointLoop();

  const PersistenceOptions options_;
  Clock* const clock_;
  std::unique_ptr<Log> log_;
  absl::Time restart_floor_ = absl::InfinitePast();

  // The commit gate (backend::DatabaseLog). Ordered before catalog_mu_.
  absl::Mutex gate_ ABSL_ACQUIRED_BEFORE(catalog_mu_);

  absl::Mutex catalog_mu_;
  uint64_t next_incarnation_ ABSL_GUARDED_BY(catalog_mu_) = 1;
  std::map<std::string, InstanceState> instances_ ABSL_GUARDED_BY(catalog_mu_);
  std::map<uint64_t, LiveDatabase> databases_ ABSL_GUARDED_BY(catalog_mu_);

  // Serializes checkpoints. Ordered before gate_.
  absl::Mutex checkpoint_mu_ ABSL_ACQUIRED_BEFORE(gate_);

  absl::Mutex thread_mu_;
  bool stop_ ABSL_GUARDED_BY(thread_mu_) = false;
  bool checkpoint_wanted_ ABSL_GUARDED_BY(thread_mu_) = false;
  std::thread thread_;
};

// Makes SIGUSR1 request a checkpoint from every PersistenceManager's thread.
void InstallCheckpointSignalHandler();

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MANAGER_H_
