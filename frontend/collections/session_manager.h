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

#ifndef STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_
#define STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "common/clock.h"
#include "frontend/collections/multiplexed_session_transaction_manager.h"
#include "frontend/common/labels.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// A session created with this label set to "true" is ephemeral: it gets its
// own copy of its database (backend::Database::CreateEphemeralCopy), which it
// alone reads and writes, and which is released with the session. With any
// other value, or without the label, a session is an ordinary one.
inline constexpr char kEphemeralSessionLabel[] = "emulator-ephemeral";

// Session manager manages the set of active sessions in the emulator.
//
// Ordinary sessions expire lazily: a lookup of one idle for an hour (28 days
// if multiplexed) finds none. Ephemeral sessions also expire after
// config::ephemeral_session_idle_timeout(), or when their database is dropped,
// and a background thread deletes those every kSweepInterval, with or without
// requests, to release their copies. Nothing is destroyed while mu_ is held:
// the last reference to a session may destroy a whole database.
class SessionManager {
 public:
  // Starts the sweeping thread.
  explicit SessionManager(Clock* clock);

  // Stops and joins the sweeping thread.
  ~SessionManager();

  // Creates a session attached to the given database. Returns
  // DatabaseNotFound if the database has been dropped. An ephemeral session
  // (kEphemeralSessionLabel) cannot be multiplexed, and fails with
  // RESOURCE_EXHAUSTED if config::max_ephemeral_sessions() are open.
  absl::StatusOr<std::shared_ptr<Session>> CreateSession(
      const Labels& labels, bool multiplexed,
      std::shared_ptr<Database> database,
      MultiplexedSessionTransactionManager* mux_txn_manager)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Returns a session with the given URI.
  absl::StatusOr<std::shared_ptr<Session>> GetSession(
      const std::string& session_uri) ABSL_LOCKS_EXCLUDED(mu_);

  // Deletes a session with the given URI.
  absl::Status DeleteSession(const std::string& session_uri,
                             bool delete_multiplex_sessions = false)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Deletes every session attached to the given database object, including
  // idle and multiplexed ones and ephemeral sessions with copies of it, so
  // that none of them keeps it alive. Sessions of another database with the
  // same URI are kept.
  void DeleteDatabaseSessions(const Database& database)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Lists sessions attached to the given database URI.
  absl::StatusOr<std::vector<std::shared_ptr<Session>>> ListSessions(
      const std::string& database_uri,
      bool include_multiplex_sessions = false) const ABSL_LOCKS_EXCLUDED(mu_);

  bool IsMultiplexedSession(const std::string& session_uri) const
      ABSL_LOCKS_EXCLUDED(mu_);

  // Deletes the ephemeral sessions expired by the server clock. The sweeping
  // thread calls it every kSweepInterval of real time.
  void SweepEphemeralSessions() ABSL_LOCKS_EXCLUDED(mu_);

  // Copies that count towards config::max_ephemeral_sessions(): those being
  // made, and those of ephemeral sessions not destroyed yet. A deleted session
  // lives on while a request in flight still holds it.
  int ephemeral_session_count() const;

  static constexpr absl::Duration kSweepInterval = absl::Seconds(10);

  // Makes the sweeping thread run every `interval` from now on. For tests.
  void set_sweep_interval_for_testing(absl::Duration interval)
      ABSL_LOCKS_EXCLUDED(sweep_mu_);

 private:
  // True if `session` has expired at `now`.
  static bool Expired(const Session& session, absl::Time now);

  // Removes the session at `itr` from the map into `released`.
  std::map<std::string, std::shared_ptr<Session>>::iterator EraseLocked(
      std::map<std::string, std::shared_ptr<Session>>::iterator itr,
      std::vector<std::shared_ptr<Session>>* released)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  absl::StatusOr<std::shared_ptr<Session>> CreateEphemeralSession(
      const Labels& labels, std::shared_ptr<Database> database)
      ABSL_LOCKS_EXCLUDED(mu_);

  void SweepLoop() ABSL_LOCKS_EXCLUDED(sweep_mu_);

  // Copies counted against config::max_ephemeral_sessions(). Shared with the
  // reservations, which sessions own, so it outlives the manager if needed.
  struct EphemeralCapacity {
    mutable absl::Mutex mu;
    int used ABSL_GUARDED_BY(mu) = 0;
  };

  // System-wide clock.
  Clock* clock_;

  // Mutex to guard state below.
  mutable absl::Mutex mu_;

  // Counter for session ids.
  int next_session_id_ ABSL_GUARDED_BY(mu_) = 0;

  // Map from session URI to session objects.
  std::map<std::string, std::shared_ptr<Session>> session_map_
      ABSL_GUARDED_BY(mu_);

  const std::shared_ptr<EphemeralCapacity> ephemeral_capacity_ =
      std::make_shared<EphemeralCapacity>();

  // Wakes the sweeping thread.
  absl::Mutex sweep_mu_;
  bool stopping_ ABSL_GUARDED_BY(sweep_mu_) = false;
  absl::Duration sweep_interval_ ABSL_GUARDED_BY(sweep_mu_) = kSweepInterval;
  // Bumped by set_sweep_interval_for_testing, to wake the thread.
  int64_t sweep_generation_ ABSL_GUARDED_BY(sweep_mu_) = 0;

  // Declared last: it starts after, and stops before, everything above.
  std::thread sweeper_;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_
