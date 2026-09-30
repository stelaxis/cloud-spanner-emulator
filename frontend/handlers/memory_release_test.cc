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

// Freed memory goes back to the operating system from the memory reclaimer's
// thread, and expired schema versions are removed without a schema change,
// through the gRPC API.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "common/config.h"
#include "common/heap_release.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/session_manager.h"
#include "frontend/converters/time.h"
#include "frontend/entities/database.h"
#include "frontend/server/memory_reclaimer.h"
#include "gmock/gmock.h"
#include "google/protobuf/empty.pb.h"
#include "google/spanner/admin/database/v1/spanner_database_admin.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "google/spanner/v1/spanner.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"
#include "tests/common/proto_matchers.h"
#include "tests/common/test_env.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace {

using ::googlesql_base::testing::IsOkAndHolds;
using ::googlesql_base::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::StartsWith;

namespace spanner_api = ::google::spanner::v1;
namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;
namespace protobuf_api = ::google::protobuf;

class MemoryReleaseTest : public test::ServerTest {
 protected:
  // Behind the wall clock, so that reads never wait for their timestamp once
  // the clock has moved past the retention period.
  MemoryReleaseTest() : ServerTest(-absl::Hours(48)) {}

  void SetUp() override {
    // The reclaimer's thread stays idle; tests do its work themselves.
    reclaimer()->set_intervals_for_testing(absl::ZeroDuration(),
                                           absl::ZeroDuration());
    GOOGLESQL_ASSERT_OK(CreateTestInstance());
    GOOGLESQL_ASSERT_OK(CreateTestDatabase());
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(session_,
                                   CreateTestSession(/*multiplexed=*/false));
  }

  MemoryReclaimer* reclaimer() { return test_env()->env()->memory_reclaimer(); }

  int64_t Requests() { return GetHeapReleaseCounts().requests; }
  int64_t Releases() { return GetHeapReleaseCounts().releases; }

  absl::Status DropDatabase() {
    grpc::ClientContext context;
    database_api::DropDatabaseRequest request;
    request.set_database(test_database_uri_);
    protobuf_api::Empty response;
    return test_env()->database_admin_client()->DropDatabase(&context, request,
                                                             &response);
  }

  absl::Status DeleteInstance() {
    grpc::ClientContext context;
    instance_api::DeleteInstanceRequest request;
    request.set_name(test_instance_uri_);
    protobuf_api::Empty response;
    return test_env()->instance_admin_client()->DeleteInstance(
        &context, request, &response);
  }

  // Applies `statement` as a schema change of its own, and returns its commit
  // timestamp.
  absl::StatusOr<absl::Time> ChangeSchema(const std::string& statement) {
    database_api::UpdateDatabaseDdlMetadata metadata;
    GOOGLESQL_RETURN_IF_ERROR(
        UpdateDatabaseDdl(test_database_uri_, {statement}, &metadata));
    return TimestampFromProto(metadata.commit_timestamps(0));
  }

  std::string CreateTable(int i) {
    return absl::StrCat("CREATE TABLE t", i,
                        " (k INT64 NOT NULL) PRIMARY KEY (k)");
  }

  backend::Database* Backend() {
    return test_env()
        ->env()
        ->database_manager()
        ->GetDatabase(test_database_uri_)
        .value()
        ->backend();
  }

  // Moves the server's clock forward to `time`, with a new session: the old
  // one may have expired.
  void MoveClockTo(absl::Time time) {
    test_env()->AdvanceClock(time - test_env()->env()->clock()->Now());
    session_ = CreateTestSession(/*multiplexed=*/false).value();
  }

  // A single-use read-only transaction reading at `timestamp`, or a strong one.
  spanner_api::TransactionSelector ReadOnly(
      std::optional<absl::Time> timestamp) {
    spanner_api::TransactionSelector selector;
    auto* read_only = selector.mutable_single_use()->mutable_read_only();
    if (timestamp.has_value()) {
      *read_only->mutable_read_timestamp() =
          TimestampToProto(*timestamp).value();
    } else {
      read_only->set_strong(true);
    }
    return selector;
  }

  absl::Status ReadTable(std::optional<absl::Time> timestamp,
                         const std::string& table) {
    spanner_api::ReadRequest request;
    request.set_session(session_);
    *request.mutable_transaction() = ReadOnly(timestamp);
    request.set_table(table);
    request.add_columns("k");
    request.mutable_key_set()->set_all(true);
    spanner_api::ResultSet response;
    return Read(request, &response);
  }

  absl::Status QueryTable(std::optional<absl::Time> timestamp,
                          const std::string& table) {
    return Query(timestamp, absl::StrCat("SELECT k FROM ", table)).status();
  }

  // The first column of every row.
  absl::StatusOr<std::vector<std::string>> Query(
      std::optional<absl::Time> timestamp, const std::string& sql) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session_);
    *request.mutable_transaction() = ReadOnly(timestamp);
    request.set_sql(sql);
    spanner_api::ResultSet response;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteSql(request, &response));
    std::vector<std::string> values;
    for (const auto& row : response.rows()) {
      values.push_back(row.values(0).string_value());
    }
    return values;
  }

  // Query, streaming; for single-column results.
  absl::StatusOr<std::vector<std::string>> StreamingQuery(
      std::optional<absl::Time> timestamp, const std::string& sql) {
    spanner_api::ExecuteSqlRequest request;
    request.set_session(session_);
    *request.mutable_transaction() = ReadOnly(timestamp);
    request.set_sql(sql);
    std::vector<spanner_api::PartialResultSet> response;
    GOOGLESQL_RETURN_IF_ERROR(ExecuteStreamingSql(request, &response));
    std::vector<std::string> values;
    for (const auto& partial : response) {
      for (const auto& value : partial.values()) {
        values.push_back(value.string_value());
      }
    }
    return values;
  }

  // Three schema changes: t1, then t2 a minute later, then t3 an hour on,
  // which removes t1's schema. Returns the time t1 was created.
  absl::StatusOr<absl::Time> RemoveASchemaByASchemaChange() {
    GOOGLESQL_ASSIGN_OR_RETURN(absl::Time t1, ChangeSchema(CreateTable(1)));
    test_env()->AdvanceClock(absl::Minutes(1));
    GOOGLESQL_ASSIGN_OR_RETURN(absl::Time t2, ChangeSchema(CreateTable(2)));
    MoveClockTo(t2 + absl::Hours(1) + absl::Seconds(10));
    GOOGLESQL_ASSIGN_OR_RETURN(absl::Time t3, ChangeSchema(CreateTable(3)));
    if (Backend()->SchemaTimestampsForTesting() !=
        std::vector<absl::Time>{absl::InfinitePast(), t2, t3}) {
      return absl::InternalError("t1's schema was not removed");
    }
    return t1;
  }

  // Reads at a time whose schema a schema change removed are resolved against
  // the schema left before it, as upstream does, however old they are.
  void ExpectReadsAsUpstreamWhereASchemaChangeRemovedTheSchema() {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time t1,
                                   RemoveASchemaByASchemaChange());
    const absl::Time too_old = t1 + absl::Seconds(30);
    EXPECT_THAT(Query(too_old, "SELECT 1"), IsOkAndHolds(ElementsAre("1")));
    EXPECT_THAT(StreamingQuery(too_old, "SELECT 1"),
                IsOkAndHolds(ElementsAre("1")));
    EXPECT_THAT(ReadTable(too_old, "t1"),
                StatusIs(absl::StatusCode::kNotFound, "Table not found: t1"));
    EXPECT_THAT(QueryTable(too_old, "t1"),
                StatusIs(absl::StatusCode::kInvalidArgument,
                         StartsWith("Table not found: t1 [at 1:15]")));
  }

  // The error a read at `timestamp` past the version retention period gets.
  static std::string TooOld(absl::Time timestamp) {
    return absl::StrCat("Read-only transaction timestamp ",
                        absl::FormatTime(timestamp, absl::UTCTimeZone()),
                        " has exceeded the maximum timestamp staleness");
  }

  // Waits up to 10 seconds for `done`.
  template <typename Predicate>
  static bool Await(Predicate done) {
    const absl::Time deadline = absl::Now() + absl::Seconds(10);
    while (!done()) {
      if (absl::Now() > deadline) return false;
      absl::SleepFor(absl::Milliseconds(5));
    }
    return true;
  }

  std::string session_;
};

TEST_F(MemoryReleaseTest, DropDatabaseAsksForAHeapRelease) {
  const int64_t requests = Requests();
  GOOGLESQL_ASSERT_OK(DropDatabase());
  EXPECT_EQ(Requests(), requests + 1);
}

TEST_F(MemoryReleaseTest, DeleteInstanceAsksForAHeapRelease) {
  const int64_t requests = Requests();
  GOOGLESQL_ASSERT_OK(DeleteInstance());
  EXPECT_EQ(Requests(), requests + 1);
}

TEST_F(MemoryReleaseTest, SchemaChangesAskForAHeapRelease) {
  const int64_t requests = Requests();
  GOOGLESQL_ASSERT_OK(ChangeSchema(CreateTable(1)));
  EXPECT_EQ(Requests(), requests + 1);

  // So does one that fails.
  EXPECT_FALSE(ChangeSchema(CreateTable(1)).ok());
  EXPECT_EQ(Requests(), requests + 2);
}

// Copies are made and freed often, and each is small.
TEST_F(MemoryReleaseTest, EphemeralCopiesDoNotAskForAHeapRelease) {
  const int64_t requests = Requests();
  SessionManager* sessions = test_env()->env()->session_manager();
  std::vector<std::weak_ptr<Database>> copies;
  std::vector<std::string> names;
  for (int i = 0; i < 3; ++i) {
    grpc::ClientContext context;
    spanner_api::CreateSessionRequest request;
    request.set_database(test_database_uri_);
    (*request.mutable_session()->mutable_labels())[kEphemeralSessionLabel] =
        "true";
    spanner_api::Session session;
    GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->CreateSession(
        &context, request, &session));
    names.push_back(session.name());
    copies.push_back(sessions->GetSession(session.name()).value()->database());
  }

  // Two deleted, one expired and swept.
  for (int i = 0; i < 2; ++i) {
    grpc::ClientContext context;
    spanner_api::DeleteSessionRequest request;
    request.set_name(names[i]);
    protobuf_api::Empty response;
    GOOGLESQL_ASSERT_OK(test_env()->spanner_client()->DeleteSession(
        &context, request, &response));
  }
  sessions->set_sweep_interval_for_testing(absl::Milliseconds(5));
  test_env()->AdvanceClock(absl::Seconds(121));
  ASSERT_TRUE(Await([&] { return sessions->ephemeral_session_count() == 0; }));

  for (const std::weak_ptr<Database>& copy : copies) {
    EXPECT_TRUE(copy.expired());
  }
  EXPECT_EQ(Requests(), requests);
}

// However many requests come between two of its rounds, the reclaimer
// releases the heap once.
TEST_F(MemoryReleaseTest, ReleasesAreCoalesced) {
  GOOGLESQL_ASSERT_OK(ChangeSchema(CreateTable(1)));
  GOOGLESQL_ASSERT_OK(ChangeSchema(CreateTable(2)));
  GOOGLESQL_ASSERT_OK(DropDatabase());
  const int64_t releases = Releases();

  reclaimer()->set_intervals_for_testing(absl::ZeroDuration(),
                                         absl::Milliseconds(10));
  ASSERT_TRUE(Await([&] { return Releases() > releases; }));
  absl::SleepFor(absl::Milliseconds(200));  // Twenty more rounds.
  EXPECT_EQ(Releases(), releases + 1);
}

// Ten schema changes a minute apart, then none: as the retention period (one
// hour) passes over them, the sweep removes every schema no read can need, and
// reads behave as if they were all still there.
TEST_F(MemoryReleaseTest, SweepRemovesExpiredSchemasWithoutASchemaChange) {
  std::vector<absl::Time> changes;
  for (int i = 1; i <= 10; ++i) {
    if (i > 1) test_env()->AdvanceClock(absl::Minutes(1));
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time change,
                                   ChangeSchema(CreateTable(i)));
    changes.push_back(change);
  }
  std::vector<absl::Time> all = {absl::InfinitePast()};
  all.insert(all.end(), changes.begin(), changes.end());
  ASSERT_THAT(Backend()->SchemaTimestampsForTesting(), ElementsAreArray(all));

  // The retention period now ends 20 seconds after the fourth change, which
  // created t4. Reads older than that fail; later ones use its schema or a
  // newer one.
  MoveClockTo(changes[3] + absl::Hours(1) + absl::Seconds(20));
  const int64_t requests = Requests();
  reclaimer()->RemoveExpiredSchemas();
  EXPECT_THAT(
      Backend()->SchemaTimestampsForTesting(),
      ElementsAre(absl::InfinitePast(), changes[3], changes[4], changes[5],
                  changes[6], changes[7], changes[8], changes[9]));
  EXPECT_EQ(Requests(), requests + 1);

  // Just inside the retention period.
  const absl::Time recent = changes[3] + absl::Seconds(40);
  GOOGLESQL_EXPECT_OK(ReadTable(recent, "t4"));
  GOOGLESQL_EXPECT_OK(QueryTable(recent, "t4"));
  EXPECT_THAT(ReadTable(recent, "t5"), StatusIs(absl::StatusCode::kNotFound));

  // Too old: the same error whether the read's schema was removed or kept.
  const absl::Time removed = changes[2] + absl::Seconds(1);
  const absl::Time kept = changes[3] + absl::Seconds(1);
  for (absl::Time too_old : {removed, kept}) {
    EXPECT_THAT(
        ReadTable(too_old, "t3"),
        StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(too_old)));
    EXPECT_THAT(
        QueryTable(too_old, "t3"),
        StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(too_old)));
  }

  // Past the last change's retention period, only it and the creation schema
  // are left.
  MoveClockTo(changes[9] + absl::Hours(1) + absl::Seconds(1));
  reclaimer()->RemoveExpiredSchemas();
  EXPECT_THAT(Backend()->SchemaTimestampsForTesting(),
              ElementsAre(absl::InfinitePast(), changes[9]));
  GOOGLESQL_EXPECT_OK(ReadTable(std::nullopt, "t10"));
  GOOGLESQL_EXPECT_OK(QueryTable(std::nullopt, "t1"));
}

// The reclaimer's thread sweeps on its own, and not at all with a zero
// interval.
TEST_F(MemoryReleaseTest, ReclaimerThreadSweepsEveryInterval) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time first,
                                 ChangeSchema(CreateTable(1)));
  test_env()->AdvanceClock(absl::Minutes(1));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time second,
                                 ChangeSchema(CreateTable(2)));
  MoveClockTo(second + absl::Hours(1) + absl::Seconds(1));

  absl::SleepFor(absl::Milliseconds(100));
  EXPECT_THAT(Backend()->SchemaTimestampsForTesting(),
              ElementsAre(absl::InfinitePast(), first, second));

  reclaimer()->set_intervals_for_testing(absl::Milliseconds(10),
                                         absl::ZeroDuration());
  EXPECT_TRUE(Await([&] {
    return Backend()->SchemaTimestampsForTesting() ==
           std::vector<absl::Time>{absl::InfinitePast(), second};
  }));
}

// A sweep removed t1's schema; the retention period is then lengthened to
// cover it again. As in Spanner, lengthening it does not bring back what was
// collected: reads at those times fail as too old, rather than being resolved
// against the schema before it.
TEST_F(MemoryReleaseTest, LengtheningRetentionDoesNotBringSweptSchemasBack) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time t1, ChangeSchema(CreateTable(1)));
  test_env()->AdvanceClock(absl::Minutes(1));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time t2, ChangeSchema(CreateTable(2)));
  MoveClockTo(t2 + absl::Hours(2));
  reclaimer()->RemoveExpiredSchemas();
  ASSERT_THAT(Backend()->SchemaTimestampsForTesting(),
              ElementsAre(absl::InfinitePast(), t2));
  GOOGLESQL_ASSERT_OK(ChangeSchema(
      absl::StrCat("ALTER DATABASE `", test_database_name_,
                   "` SET OPTIONS (version_retention_period = '1d')")));

  const absl::Time swept = t1 + absl::Seconds(30);
  EXPECT_THAT(ReadTable(swept, "t1"),
              StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(swept)));
  EXPECT_THAT(QueryTable(swept, "t1"),
              StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(swept)));
  EXPECT_THAT(Query(swept,
                    "SELECT table_name FROM information_schema.tables "
                    "WHERE table_schema = ''"),
              StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(swept)));
  EXPECT_THAT(StreamingQuery(swept, "SELECT 1"),
              StatusIs(absl::StatusCode::kFailedPrecondition, TooOld(swept)));

  // Times whose schema is still there read as before: t2's, and those before
  // t1, when the database's first schema was in effect.
  GOOGLESQL_EXPECT_OK(ReadTable(t2 + absl::Seconds(30), "t1"));
  EXPECT_THAT(ReadTable(t1 - absl::Seconds(1), "t1"),
              StatusIs(absl::StatusCode::kNotFound, "Table not found: t1"));
  EXPECT_THAT(Query(t1 - absl::Seconds(1), "SELECT COUNT(*) FROM test_table"),
              IsOkAndHolds(ElementsAre("0")));
}

TEST_F(MemoryReleaseTest, SchemaChangesRemoveSchemasAsUpstreamDoes) {
  ExpectReadsAsUpstreamWhereASchemaChangeRemovedTheSchema();
}

// A sweep lasting most of a release interval does not bring the next release
// closer: releases stay at least an interval apart.
TEST_F(MemoryReleaseTest, ReleasesStayAnIntervalApartAfterASlowSweep) {
  struct State {
    absl::Mutex mu;
    int sweeps ABSL_GUARDED_BY(mu) = 0;
    std::vector<absl::Time> releases ABSL_GUARDED_BY(mu);
  };
  auto state = std::make_shared<State>();
  constexpr absl::Duration kInterval = absl::Milliseconds(300);
  reclaimer()->set_hooks_for_testing(
      /*before_sweep=*/
      [state, kInterval] {
        bool first;
        {
          absl::MutexLock lock(state->mu);
          first = state->sweeps++ == 0;
        }
        if (first) absl::SleepFor(kInterval - absl::Milliseconds(50));
        RequestHeapRelease();
      },
      /*after_release=*/
      [state] {
        absl::MutexLock lock(state->mu);
        state->releases.push_back(absl::Now());
      });
  reclaimer()->set_intervals_for_testing(kInterval, kInterval);
  ASSERT_TRUE(Await([&] {
    absl::MutexLock lock(state->mu);
    return state->releases.size() >= 3;
  }));
  reclaimer()->set_intervals_for_testing(absl::ZeroDuration(),
                                         absl::ZeroDuration());
  reclaimer()->set_hooks_for_testing(nullptr, nullptr);

  absl::MutexLock lock(state->mu);
  for (int i = 1; i < state->releases.size(); ++i) {
    EXPECT_GE(state->releases[i] - state->releases[i - 1], kInterval)
        << "release " << i;
  }
}

// With both flags 0 the emulator neither sweeps nor releases, and reads are
// exactly upstream's.
class MemoryReleaseDisabledTest : public MemoryReleaseTest {
 protected:
  static void SetUpTestSuite() {
    config::set_heap_release_interval(absl::ZeroDuration());
    config::set_schema_version_gc_interval(absl::ZeroDuration());
  }
  static void TearDownTestSuite() {
    config::set_heap_release_interval(absl::Seconds(10));
    config::set_schema_version_gc_interval(absl::Seconds(60));
  }
};

TEST_F(MemoryReleaseDisabledTest, ReadsAreUpstreams) {
  ExpectReadsAsUpstreamWhereASchemaChangeRemovedTheSchema();
}

}  // namespace
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
