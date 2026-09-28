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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
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
// and MaybeSweepEphemeralSessions deletes those without a lookup, to release
// their copies. Nothing is destroyed while mu_ is held: the last reference to a
// session may destroy a whole database.
class SessionManager {
 public:
  explicit SessionManager(Clock* clock) : clock_(clock) {}

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

  // Deletes the expired ephemeral sessions, unless it did so less than
  // kSweepInterval ago by the server clock. Every request calls it.
  void MaybeSweepEphemeralSessions() ABSL_LOCKS_EXCLUDED(mu_);

  // Ephemeral sessions open, and being created.
  int ephemeral_session_count() const ABSL_LOCKS_EXCLUDED(mu_);

  static constexpr absl::Duration kSweepInterval = absl::Seconds(10);

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

  // System-wide clock.
  Clock* clock_;

  // Mutex to guard state below.
  mutable absl::Mutex mu_;

  // Counter for session ids.
  int next_session_id_ ABSL_GUARDED_BY(mu_) = 0;

  // Map from session URI to session objects.
  std::map<std::string, std::shared_ptr<Session>> session_map_
      ABSL_GUARDED_BY(mu_);

  // Ephemeral sessions in session_map_, plus those being created.
  int ephemeral_sessions_ ABSL_GUARDED_BY(mu_) = 0;

  // Server time, in microseconds, before which MaybeSweepEphemeralSessions
  // does nothing.
  std::atomic<int64_t> next_sweep_micros_ = 0;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // STORAGE_CLOUD_SPANNER_EMULATOR_FRONTEND_SESSION_MANAGER_H_
