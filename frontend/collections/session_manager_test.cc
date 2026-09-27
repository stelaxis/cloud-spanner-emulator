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

#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "common/clock.h"
#include "common/errors.h"
#include "frontend/collections/database_manager.h"
#include "frontend/entities/database.h"
#include "frontend/entities/session.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

class SessionManagerTest : public testing::Test {
 protected:
  SessionManagerTest()
      : session_manager_(&clock_), database_manager_(&clock_) {}

  void SetUp() override {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        database_,
        database_manager_.CreateDatabase(
            "projects/test-p/instances/test-i/databases/test-database", {}));
  }

  Clock clock_;
  Labels test_labels_;
  bool multiplexed_ = false;
  SessionManager session_manager_;
  DatabaseManager database_manager_;
  std::shared_ptr<Database> database_;
};

TEST_F(SessionManagerTest, CreateSession) {
  // The emulator clock follows the system clock at microsecond granularity, so
  // a session created in the same microsecond as `start_time` carries that
  // microsecond, which can be earlier than `start_time` itself.
  absl::Time start_time = absl::FromUnixMicros(absl::ToUnixMicros(absl::Now()));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> actual,
      session_manager_.CreateSession(test_labels_, multiplexed_, database_,
                                     /*mux_txn_manager=*/nullptr));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> actual_multiplexed,
      session_manager_.CreateSession(test_labels_, true, database_,
                                     /*mux_txn_manager=*/nullptr));
  EXPECT_TRUE(
      absl::StartsWith(actual->session_uri(), database_->database_uri()));
  EXPECT_GE(actual->create_time(), start_time);
  EXPECT_GE(actual->approximate_last_use_time(), actual->create_time());

  EXPECT_TRUE(absl::StartsWith(actual_multiplexed->session_uri(),
                               database_->database_uri()));
  EXPECT_TRUE(actual_multiplexed->multiplexed());
  EXPECT_GT(actual_multiplexed->create_time(), actual->create_time());
  EXPECT_GE(actual_multiplexed->approximate_last_use_time(),
            actual_multiplexed->create_time());
}

TEST_F(SessionManagerTest, GetSession) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_, multiplexed_, database_,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Session> actual,
                       session_manager_.GetSession(expected->session_uri()));
  EXPECT_EQ(actual->session_uri(), expected->session_uri());
  EXPECT_EQ(actual->create_time(), expected->create_time());
  EXPECT_EQ(actual->approximate_last_use_time(),
            expected->approximate_last_use_time());
}

TEST_F(SessionManagerTest, GetSessionIncludesMultiplexedFlag) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_, true, database_,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Session> actual,
                       session_manager_.GetSession(expected->session_uri()));
  EXPECT_TRUE(actual->multiplexed());
  EXPECT_EQ(actual->session_uri(), expected->session_uri());
  EXPECT_EQ(actual->create_time(), expected->create_time());
  EXPECT_EQ(actual->approximate_last_use_time(),
            expected->approximate_last_use_time());
}

TEST_F(SessionManagerTest, GetSessionFailsAfterVersionGcDuration) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_, multiplexed_, database_,
                                     /*mux_txn_manager=*/nullptr));
  // Set the approximate_last_use_time to earlier than gc duration.
  expected->set_approximate_last_use_time(absl::Now() - absl::Hours(1.5));
  EXPECT_THAT(session_manager_.GetSession(expected->session_uri()),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(SessionManagerTest, GetMultiplexedSessionFailsAfterVersionGcDuration) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_, true, database_,
                                     /*mux_txn_manager=*/nullptr));
  // Set the approximate_last_use_time to earlier than regular session gc
  // duration.
  expected->set_approximate_last_use_time(absl::Now() - absl::Hours(1.5));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Session> actual,
                       session_manager_.GetSession(expected->session_uri()));
  EXPECT_TRUE(actual->multiplexed());
  // Set the approximate_last_use_time to earlier than multiplexed session gc
  // duration.
  expected->set_approximate_last_use_time(absl::Now() - absl::Hours(28.5 * 24));
  EXPECT_THAT(session_manager_.GetSession(expected->session_uri()),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(SessionManagerTest, DeleteSession) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> actual,
      session_manager_.CreateSession(test_labels_, multiplexed_, database_,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_EXPECT_OK(session_manager_.DeleteSession(actual->session_uri()));
  EXPECT_THAT(session_manager_.GetSession(actual->session_uri()),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(SessionManagerTest, ListSessions) {
  int num = 5;
  std::shared_ptr<Session> expected;
  for (int i = 0; i < num; i++) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(expected, session_manager_.CreateSession(
                                       test_labels_, multiplexed_, database_,
                                       /*mux_txn_manager=*/nullptr));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<std::shared_ptr<Session>> actual,
      session_manager_.ListSessions(database_->database_uri()));
  EXPECT_EQ(actual.size(), num);
  for (int i = 0; i < num; i++) {
    EXPECT_TRUE(
        absl::StartsWith(actual[i]->session_uri(), database_->database_uri()));
  }
}

TEST_F(SessionManagerTest, ListSessionsIncludesMultiplexedFlag) {
  int num = 5;
  std::shared_ptr<Session> expected;
  for (int i = 0; i < num; i++) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(expected, session_manager_.CreateSession(
                                       test_labels_,
                                       /*multiplexed=*/true, database_,
                                       /*mux_txn_manager=*/nullptr));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<std::shared_ptr<Session>> actual,
      session_manager_.ListSessions(database_->database_uri()));
  // ListSessions API does not return any multiplexed sessions.
  EXPECT_EQ(actual.size(), 0);
}

TEST_F(SessionManagerTest, ListSessionsReturnsMultiplexedSessions) {
  int num = 5;
  std::shared_ptr<Session> expected;
  for (int i = 0; i < num; i++) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(expected, session_manager_.CreateSession(
                                       test_labels_,
                                       /*multiplexed=*/true, database_,
                                       /*mux_txn_manager=*/nullptr));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<std::shared_ptr<Session>> actual,
      session_manager_.ListSessions(database_->database_uri(),
                                    /*include_multiplex_sessions=*/true));
  EXPECT_EQ(actual.size(), 5);
}

TEST_F(SessionManagerTest,
       ListSessionsReturnsMultiplexedSessionsAfterGcDuration) {
  std::shared_ptr<Session> expected;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      expected, session_manager_.CreateSession(test_labels_,
                                               /*multiplexed=*/true, database_,
                                               /*mux_txn_manager=*/nullptr));
  // Set the approximate_last_use_time to earlier than multiplexed session gc
  // duration.
  expected->set_approximate_last_use_time(absl::Now() - absl::Minutes(90));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<std::shared_ptr<Session>> actual,
      session_manager_.ListSessions(database_->database_uri(),
                                    /*include_multiplex_sessions=*/true));
  EXPECT_EQ(actual.size(), 1);
}

TEST_F(SessionManagerTest,
       DeleteSessionIncludesMultiplexedFlagAndDropMultiplexSessionsIsTrue) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_,
                                     /*multiplexed=*/true, database_,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Session> actual,
                       session_manager_.GetSession(expected->session_uri()));
  EXPECT_EQ(actual, expected);

  // DeleteSession API with delete_multiplex_sessions = true deletes multiplexed
  // sessions.
  GOOGLESQL_EXPECT_OK(session_manager_.DeleteSession(expected->session_uri(),
                                           /*delete_multiplex_sessions=*/true));
  EXPECT_THAT(session_manager_.GetSession(expected->session_uri()),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(SessionManagerTest,
       DeleteSessionIncludesMultiplexedFlagAndDropMultiplexSessionsIsFalse) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> expected,
      session_manager_.CreateSession(test_labels_,
                                     /*multiplexed=*/true, database_,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Session> actual,
                       session_manager_.GetSession(expected->session_uri()));
  EXPECT_EQ(actual, expected);

  // DeleteSession API with delete_multiplex_sessions = false does not delete
  // multiplexed sessions.
  EXPECT_EQ(session_manager_.DeleteSession(expected->session_uri()),
            error::InvalidOperationSessionDelete());
}

TEST_F(SessionManagerTest, ListSessionsWithSimilarPrefix) {
  int num = 5;
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database1,
      database_manager_.CreateDatabase(
          "projects/test-p/instances/test-i/databases/test", {}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> database2,
      database_manager_.GetDatabase(
          "projects/test-p/instances/test-i/databases/test-database"));

  std::shared_ptr<Session> expected;
  for (int i = 0; i < num; i++) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(expected, session_manager_.CreateSession(
                                       test_labels_, multiplexed_, database1,
                                       /*mux_txn_manager=*/nullptr));
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(expected, session_manager_.CreateSession(
                                     test_labels_, multiplexed_, database2,
                                     /*mux_txn_manager=*/nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::vector<std::shared_ptr<Session>> actual,
      session_manager_.ListSessions(database1->database_uri()));
  EXPECT_EQ(actual.size(), num);
  for (int i = 0; i < num; i++) {
    EXPECT_TRUE(absl::StartsWith(
        actual[i]->session_uri(),
        absl::StrCat(database1->database_uri(), "/sessions/")));
  }
}

TEST_F(SessionManagerTest, CreateSessionFailsOnDroppedDatabase) {
  database_->MarkDropped();
  EXPECT_THAT(
      session_manager_.CreateSession(test_labels_, multiplexed_, database_,
                                     /*mux_txn_manager=*/nullptr),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST(SessionManagerDropTest, DeleteDatabaseSessionsReleasesTheDatabase) {
  absl::Time now = absl::FromUnixSeconds(1700000000);
  Clock clock([&now] { return now; });
  SessionManager session_manager(&clock);
  DatabaseManager database_manager(&clock);
  const std::string uri = "projects/test-p/instances/test-i/databases/db";
  const std::string similar_uri = uri + "2";
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::shared_ptr<Database> database,
                                 database_manager.CreateDatabase(uri, {}));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Database> similar_database,
      database_manager.CreateDatabase(similar_uri, {}));
  GOOGLESQL_ASSERT_OK(session_manager.CreateSession({}, /*multiplexed=*/false,
                                                    database, nullptr));
  // Past the one-hour expiration of idle sessions.
  now += absl::Hours(2);
  GOOGLESQL_ASSERT_OK(session_manager.CreateSession({}, /*multiplexed=*/false,
                                                    database, nullptr));
  GOOGLESQL_ASSERT_OK(session_manager.CreateSession({}, /*multiplexed=*/true,
                                                    database, nullptr));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Session> other,
      session_manager.CreateSession({}, /*multiplexed=*/false, similar_database,
                                    nullptr));
  std::weak_ptr<Database> released = database;
  database.reset();
  GOOGLESQL_ASSERT_OK(database_manager.DeleteDatabase(uri));

  session_manager.DeleteDatabaseSessions(uri);

  EXPECT_TRUE(released.expired());
  GOOGLESQL_EXPECT_OK(session_manager.GetSession(other->session_uri()));
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
