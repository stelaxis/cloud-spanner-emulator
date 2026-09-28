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

#include "frontend/collections/session_manager.h"

#include <map>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "common/config.h"
#include "common/errors.h"
#include "frontend/collections/multiplexed_session_transaction_manager.h"
#include "frontend/common/labels.h"
#include "frontend/common/uris.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

bool IsEphemeral(const Labels& labels) {
  auto itr = labels.find(kEphemeralSessionLabel);
  return itr != labels.end() && itr->second == "true";
}

}  // namespace

SessionManager::SessionManager(Clock* clock)
    : clock_(clock), sweeper_([this] { SweepLoop(); }) {}

SessionManager::~SessionManager() {
  {
    absl::MutexLock lock(sweep_mu_);
    stopping_ = true;
  }
  sweeper_.join();
}

void SessionManager::SweepLoop() {
  while (true) {
    {
      absl::MutexLock lock(sweep_mu_);
      const int64_t generation = sweep_generation_;
      auto woken = [this, generation]() ABSL_NO_THREAD_SAFETY_ANALYSIS {
        return stopping_ || sweep_generation_ != generation;
      };
      // Woken early only to stop or to take a new interval.
      if (sweep_mu_.AwaitWithTimeout(absl::Condition(&woken),
                                     sweep_interval_)) {
        if (stopping_) {
          return;
        }
        continue;
      }
    }
    SweepEphemeralSessions();
  }
}

void SessionManager::set_sweep_interval_for_testing(absl::Duration interval) {
  absl::MutexLock lock(sweep_mu_);
  sweep_interval_ = interval;
  ++sweep_generation_;
}

bool SessionManager::Expired(const Session& session, absl::Time now) {
  const absl::Duration idle = now - session.approximate_last_use_time();
  if (session.ephemeral()) {
    return session.database_dropped() ||
           idle > config::ephemeral_session_idle_timeout();
  }
  return idle > (session.multiplexed() ? absl::Hours(28 * 24) : absl::Hours(1));
}

std::map<std::string, std::shared_ptr<Session>>::iterator
SessionManager::EraseLocked(
    std::map<std::string, std::shared_ptr<Session>>::iterator itr,
    std::vector<std::shared_ptr<Session>>* released) {
  released->push_back(std::move(itr->second));
  return session_map_.erase(itr);
}

absl::StatusOr<std::shared_ptr<Session>> SessionManager::CreateSession(
    const Labels& labels, const bool multiplexed,
    std::shared_ptr<Database> database,
    MultiplexedSessionTransactionManager* mux_txn_manager) {
  if (IsEphemeral(labels)) {
    // Multiplexed-session transactions are kept by database URI, which every
    // copy of a database shares.
    if (multiplexed) {
      return error::EphemeralMultiplexedSession();
    }
    return CreateEphemeralSession(labels, std::move(database));
  }
  absl::MutexLock lock(mu_);
  // DeleteDatabaseSessions takes mu_ after the flag is set, so a session
  // either sees the flag here or is deleted there.
  if (database->dropped()) {
    return error::DatabaseNotFound(database->database_uri());
  }
  const std::string session_id = absl::StrCat(next_session_id_++);
  std::string session_uri =
      MakeSessionUri(database->database_uri(), session_id);
  std::shared_ptr<Session> session = std::make_shared<Session>(
      session_uri, labels, multiplexed,
      /* create_time = */ clock_->Now(), database, mux_txn_manager);
  session->set_approximate_last_use_time(clock_->Now());

  // Need to also cache mux session since the session IDs generated are not
  // from Tokens but rather just an incrementing value.
  session_map_[session_uri] = session;
  return session;
}

absl::StatusOr<std::shared_ptr<Session>> SessionManager::CreateEphemeralSession(
    const Labels& labels, std::shared_ptr<Database> database) {
  if (database->dropped()) {
    return error::DatabaseNotFound(database->database_uri());
  }
  // The copy's place, from before it is made until it is destroyed: the
  // session owns the reservation and releases it after its database.
  std::shared_ptr<void> reservation;
  {
    absl::MutexLock lock(ephemeral_capacity_->mu);
    const int max_sessions = config::max_ephemeral_sessions();
    if (ephemeral_capacity_->used >= max_sessions) {
      return error::TooManyEphemeralSessions(max_sessions);
    }
    ++ephemeral_capacity_->used;
    reservation =
        std::shared_ptr<void>(nullptr, [capacity = ephemeral_capacity_](void*) {
          absl::MutexLock lock(capacity->mu);
          --capacity->used;
        });
  }
  // Released after mu_, like everything below.
  std::shared_ptr<Database> copy;
  absl::StatusOr<std::unique_ptr<backend::Database>> backend_copy =
      database->backend()->CreateEphemeralCopy();
  if (backend_copy.ok()) {
    copy = std::make_shared<Database>(database->database_uri(),
                                      *std::move(backend_copy),
                                      database->create_time());
  }

  absl::MutexLock lock(mu_);
  if (!backend_copy.ok()) {
    return error::EphemeralSessionCopyFailed(database->database_uri(),
                                             backend_copy.status());
  }
  // As in CreateSession: a drop that swept the sessions meanwhile did not see
  // this one.
  if (database->dropped()) {
    return error::DatabaseNotFound(database->database_uri());
  }
  const std::string session_uri = MakeSessionUri(
      database->database_uri(), absl::StrCat(next_session_id_++));
  auto session = std::make_shared<Session>(
      session_uri, labels, /*multiplexed=*/false,
      /*create_time=*/clock_->Now(), std::move(copy),
      /*mux_txn_manager=*/nullptr, std::move(database), std::move(reservation));
  session->set_approximate_last_use_time(clock_->Now());
  session_map_[session_uri] = session;
  return session;
}

absl::StatusOr<std::shared_ptr<Session>> SessionManager::GetSession(
    const std::string& session_uri) {
  // Released after mu_.
  std::vector<std::shared_ptr<Session>> released;
  absl::MutexLock lock(mu_);
  auto itr = session_map_.find(session_uri);
  if (itr == session_map_.end()) {
    return error::SessionNotFound(session_uri);
  }
  std::shared_ptr<Session> session = itr->second;
  // Its database's drop deletes it, maybe not yet.
  if (session->database_dropped()) {
    return error::SessionNotFound(session_uri);
  }
  if (Expired(*session, clock_->Now())) {
    // Delete inactive sessions after expiration duration.
    EraseLocked(itr, &released);
    return error::SessionNotFound(session_uri);
  }
  session->set_approximate_last_use_time(clock_->Now());
  return session;
}

absl::StatusOr<std::vector<std::shared_ptr<Session>>>
SessionManager::ListSessions(const std::string& database_uri,
                             bool include_multiplex_sessions) const {
  absl::MutexLock lock(mu_);
  std::string session_uri_prefix = absl::StrCat(database_uri, "/");
  std::vector<std::shared_ptr<Session>> sessions;
  const absl::Time now = clock_->Now();
  for (auto itr = session_map_.lower_bound(session_uri_prefix);
       itr != session_map_.end(); ++itr) {
    if (absl::StartsWith(itr->first, session_uri_prefix)) {
      std::shared_ptr<Session> session = itr->second;
      if (session->multiplexed() && !include_multiplex_sessions) {
        // Multiplexed sessions are not sent in ListSessions response by
        // default.
        continue;
      }
      // Multiplexed session doesn't have expiration duration, so we don't check
      // for expiration here.
      if (session->multiplexed() && include_multiplex_sessions) {
        sessions.push_back(session);
      } else if (session->ephemeral()) {
        if (!Expired(*session, now)) {
          sessions.push_back(session);
        }
      } else if (now - session->approximate_last_use_time() <= absl::Hours(1)) {
        sessions.push_back(session);
      }
    } else {
      return sessions;
    }
  }
  return sessions;
}

void SessionManager::DeleteDatabaseSessions(const Database& database) {
  const std::string session_uri_prefix =
      absl::StrCat(database.database_uri(), "/");
  // Released after mu_: the last session may destroy the whole database.
  std::vector<std::shared_ptr<Session>> deleted;
  absl::MutexLock lock(mu_);
  auto itr = session_map_.lower_bound(session_uri_prefix);
  while (itr != session_map_.end() &&
         absl::StartsWith(itr->first, session_uri_prefix)) {
    if (itr->second->database().get() != &database &&
        itr->second->base_database().get() != &database) {
      ++itr;
      continue;
    }
    itr = EraseLocked(itr, &deleted);
  }
}

absl::Status SessionManager::DeleteSession(const std::string& session_uri,
                                           bool delete_multiplex_sessions) {
  // Released after mu_.
  std::vector<std::shared_ptr<Session>> released;
  absl::MutexLock lock(mu_);
  auto itr = session_map_.find(session_uri);
  if (itr != session_map_.end()) {
    // Multiplexed sessions cannot be deleted using DeleteSession API, but they
    // need to be deleted when the database is dropped.
    if (itr->second->multiplexed() && !delete_multiplex_sessions) {
      return error::InvalidOperationSessionDelete();
    }
    EraseLocked(itr, &released);
  }
  return absl::OkStatus();
}

void SessionManager::SweepEphemeralSessions() {
  if (ephemeral_session_count() == 0) {
    return;
  }
  const absl::Time now = clock_->Now();
  // Released after mu_.
  std::vector<std::shared_ptr<Session>> released;
  absl::MutexLock lock(mu_);
  for (auto itr = session_map_.begin(); itr != session_map_.end();) {
    if (itr->second->ephemeral() && Expired(*itr->second, now)) {
      itr = EraseLocked(itr, &released);
    } else {
      ++itr;
    }
  }
}

int SessionManager::ephemeral_session_count() const {
  absl::MutexLock lock(ephemeral_capacity_->mu);
  return ephemeral_capacity_->used;
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
