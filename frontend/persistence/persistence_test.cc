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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/cord.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/access/read.h"
#include "backend/access/write.h"
#include "backend/database/database.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/printer/print_ddl.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/storage/iterator.h"
#include "backend/transaction/options.h"
#include "common/clock.h"
#include "frontend/collections/database_manager.h"
#include "frontend/collections/instance_manager.h"
#include "frontend/entities/database.h"
#include "frontend/persistence/manager.h"
#include "frontend/persistence/mem_file_system.h"
#include "frontend/server/environment.h"
#include "gmock/gmock.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/text_format.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/value.h"
#include "gtest/gtest.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {
namespace {

using ::googlesql::values::Int64;
using ::googlesql::values::String;
using ::googlesql_base::testing::StatusIs;
using Op = MemFileSystem::Op;
namespace database_api = ::google::spanner::admin::database::v1;

constexpr char kInstance[] = "projects/p/instances/i1";
constexpr char kDatabase[] = "projects/p/instances/i1/databases/db1";

const std::vector<std::string> kSchema = {
    "CREATE SEQUENCE seq OPTIONS (sequence_kind = 'bit_reversed_positive')",
    R"(CREATE TABLE Accounts (
         Id INT64 NOT NULL DEFAULT (GET_NEXT_SEQUENCE_VALUE(SEQUENCE seq)),
         Name STRING(MAX) NOT NULL,
         Balance NUMERIC,
         Meta JSON,
         Tags ARRAY<STRING(MAX)>,
         Upper STRING(MAX) AS (UPPER(Name)) STORED,
         LastEntrySeq INT64,
         CONSTRAINT PositiveBalance CHECK (Balance >= 0),
       ) PRIMARY KEY (Id))",
    "CREATE UNIQUE INDEX AccountsByName ON Accounts(Name)",
    R"(CREATE TABLE Entries (
         Id INT64 NOT NULL,
         Seq INT64 NOT NULL,
         Amount FLOAT64,
         Day DATE,
       ) PRIMARY KEY (Id, Seq DESC), INTERLEAVE IN PARENT Accounts ON DELETE CASCADE)",
    R"(CREATE TABLE Transfers (
         TransferId STRING(36) NOT NULL,
         FromId INT64 NOT NULL,
         ToId INT64,
         Raw BYTES(MAX),
         CONSTRAINT FromAccount FOREIGN KEY (FromId) REFERENCES Accounts (Id),
         FOREIGN KEY (ToId) REFERENCES Accounts (Id),
       ) PRIMARY KEY (TransferId))",
    // A parent referencing its interleaved child: printed inline, this
    // foreign key refers to a table defined after it.
    R"(ALTER TABLE Accounts ADD CONSTRAINT LastEntry
         FOREIGN KEY (Id, LastEntrySeq) REFERENCES Entries (Id, Seq))",
};

// One emulator process: its clock, managers and persistence.
struct Emulator {
  std::unique_ptr<Clock> clock;
  std::unique_ptr<InstanceManager> instances;
  std::unique_ptr<DatabaseManager> databases;
  std::unique_ptr<PersistenceManager> persistence;

  ~Emulator() {
    databases.reset();
    instances.reset();
    persistence.reset();
  }
};

class PersistenceTest : public ::testing::Test {
 protected:
  absl::StatusOr<std::unique_ptr<Emulator>> Start(
      std::function<absl::Time()> system_now = nullptr) {
    auto emulator = std::make_unique<Emulator>();
    emulator->clock = system_now ? std::make_unique<Clock>(system_now)
                                 : std::make_unique<Clock>();
    emulator->instances = std::make_unique<InstanceManager>();
    emulator->databases =
        std::make_unique<DatabaseManager>(emulator->clock.get());
    PersistenceOptions options;
    options.fs = &fs_;
    options.dir = "/data";
    options.background_checkpoints = false;
    auto manager = PersistenceManager::Open(options, emulator->clock.get(),
                                            emulator->instances.get(),
                                            emulator->databases.get());
    if (!manager.ok()) return manager.status();
    emulator->persistence = *std::move(manager);
    return emulator;
  }

  // A system clock that never advances: the emulator's clock then steps by
  // one microsecond per timestamp and never needs to extend its lease, so a
  // broken log cannot stop the emulator in the middle of a test.
  static absl::Time FrozenSystemClock() {
    return absl::FromUnixSeconds(1'000'000'000);
  }

  std::unique_ptr<Emulator> StartOrDie(
      std::function<absl::Time()> system_now = nullptr) {
    auto emulator = Start(system_now);
    EXPECT_TRUE(emulator.ok()) << emulator.status();
    return emulator.ok() ? *std::move(emulator) : nullptr;
  }

  // kill -9, and the machine loses whatever was not synced.
  void Crash(std::unique_ptr<Emulator>& emulator) {
    emulator.reset();
    fs_.SetHook(nullptr);
    fs_.Crash();
  }

  std::shared_ptr<Database> CreateDatabase(
      Emulator* emulator, const std::string& uri = kDatabase,
      const std::vector<std::string>& schema = kSchema) {
    if (!emulator->instances->GetInstance(kInstance).ok()) {
      admin::instance::v1::Instance instance;
      instance.set_config("projects/p/instanceConfigs/emulator-config");
      instance.set_display_name("i1");
      instance.set_node_count(1);
      GOOGLESQL_EXPECT_OK(
          emulator->instances->CreateInstance(kInstance, instance).status());
    }
    auto database = emulator->databases->CreateDatabase(
        uri, backend::SchemaChangeOperation{
                 .statements = schema,
                 .database_dialect =
                     database_api::DatabaseDialect::GOOGLE_STANDARD_SQL});
    EXPECT_TRUE(database.ok()) << database.status();
    return database.ok() ? *database : nullptr;
  }

  std::shared_ptr<Database> GetDatabase(Emulator* emulator,
                                        const std::string& uri = kDatabase) {
    auto database = emulator->databases->GetDatabase(uri);
    EXPECT_TRUE(database.ok()) << database.status();
    return database.ok() ? *database : nullptr;
  }

  absl::StatusOr<absl::Time> Commit(Database* database,
                                    const backend::Mutation& mutation) {
    auto txn = database->backend()->CreateReadWriteTransaction(
        backend::ReadWriteOptions(), backend::RetryState());
    if (!txn.ok()) return txn.status();
    if (auto status = (*txn)->Write(mutation); !status.ok()) return status;
    if (auto status = (*txn)->Commit(); !status.ok()) return status;
    return (*txn)->GetCommitTimestamp();
  }

  absl::Status UpdateSchema(Database* database,
                            std::vector<std::string> statements) {
    int successful;
    absl::Time timestamp;
    absl::Status backfill;
    auto status = database->backend()->UpdateSchema(
        backend::SchemaChangeOperation{
            .statements = std::move(statements),
            .database_dialect =
                database_api::DatabaseDialect::GOOGLE_STANDARD_SQL},
        &successful, &timestamp, &backfill);
    return status.ok() ? backfill : status;
  }

  backend::Mutation InsertAccount(const std::string& name, int64_t balance) {
    backend::Mutation m;
    m.AddWriteOp(backend::MutationOpType::kInsert, "Accounts",
                 {"Name", "Balance"},
                 {{String(name), googlesql::values::Numeric(balance)}});
    return m;
  }

  // Every row of `table` visible to a strong read, with all its columns.
  std::vector<std::string> ReadTable(Database* database,
                                     const std::string& table) {
    auto txn = database->backend()->CreateReadOnlyTransaction(
        backend::ReadOnlyOptions());
    EXPECT_TRUE(txn.ok()) << txn.status();
    if (!txn.ok()) return {};
    const backend::Table* t =
        database->backend()->GetLatestSchema()->FindTable(table);
    backend::ReadArg arg;
    arg.table = table;
    arg.key_set = backend::KeySet::All();
    for (const backend::Column* column : t->columns()) {
      arg.columns.push_back(column->Name());
    }
    std::unique_ptr<backend::RowCursor> cursor;
    GOOGLESQL_EXPECT_OK((*txn)->Read(arg, &cursor));
    std::vector<std::string> rows;
    while (cursor->Next()) {
      std::vector<std::string> values;
      for (int i = 0; i < cursor->NumColumns(); ++i) {
        values.push_back(cursor->ColumnValue(i).DebugString());
      }
      rows.push_back(absl::StrJoin(values, ","));
    }
    GOOGLESQL_EXPECT_OK(cursor->Status());
    return rows;
  }

  // The latest version of every stored row, index tables included, by
  // storage ID: what the checkpoint and the log must reproduce exactly.
  std::vector<std::string> DumpStorage(Database* database) {
    std::vector<std::string> dump;
    std::shared_ptr<const backend::Schema> schema =
        database->backend()->GetLatestSchemaShared();
    std::vector<const backend::Table*> tables;
    for (const backend::Table* table : schema->tables()) {
      tables.push_back(table);
      for (const backend::Index* index : table->indexes()) {
        tables.push_back(index->index_data_table());
      }
    }
    absl::Time now = absl::InfiniteFuture();
    for (const backend::Table* table : tables) {
      std::vector<std::string> column_ids;
      for (const backend::Column* column : table->columns()) {
        column_ids.push_back(column->id());
      }
      std::unique_ptr<backend::StorageIterator> itr;
      GOOGLESQL_EXPECT_OK(database->backend()->storage()->Read(
          now, table->id(),
          backend::KeyRange::ClosedOpen(backend::Key::Empty(),
                                        backend::Key::Infinity()),
          column_ids, &itr));
      while (itr->Next()) {
        std::string row =
            absl::StrCat(table->id(), " ", itr->Key().DebugString());
        for (int i = 0; i < itr->NumColumns(); ++i) {
          absl::StrAppend(&row, " ", column_ids[i], "=",
                          itr->ColumnValue(i).is_valid()
                              ? itr->ColumnValue(i).DebugString()
                              : "<unset>");
        }
        dump.push_back(row);
      }
    }
    return dump;
  }

  std::vector<std::string> Ddl(Database* database) {
    auto ddl = backend::PrintDDLStatements(
        database->backend()->GetLatestSchemaShared().get());
    GOOGLESQL_EXPECT_OK(ddl.status());
    return ddl.ok() ? *ddl : std::vector<std::string>{};
  }

  void FailOn(Op failing) {
    fs_.SetHook([failing](Op op, const std::string&) {
      return op == failing ? absl::InternalError("injected I/O error")
                           : absl::OkStatus();
    });
  }

  MemFileSystem fs_;
};

TEST_F(PersistenceTest, SchemaAndRowsSurviveACrash) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  for (int i = 0; i < 5; ++i) {
    GOOGLESQL_ASSERT_OK(
        Commit(db.get(), InsertAccount(absl::StrCat("n", i), i)).status());
  }
  auto accounts = ReadTable(db.get(), "Accounts");
  ASSERT_EQ(accounts.size(), 5);
  backend::Mutation m;
  // Children, FK rows and a delete of one account's row plus its cascade.
  {
    auto txn =
        db->backend()->CreateReadOnlyTransaction(backend::ReadOnlyOptions());
    GOOGLESQL_ASSERT_OK(txn.status());
    backend::ReadArg arg{.table = "Accounts",
                         .key_set = backend::KeySet::All(),
                         .columns = {"Id"}};
    std::unique_ptr<backend::RowCursor> cursor;
    GOOGLESQL_ASSERT_OK((*txn)->Read(arg, &cursor));
    std::vector<googlesql::Value> ids;
    while (cursor->Next()) ids.push_back(cursor->ColumnValue(0));
    ASSERT_EQ(ids.size(), 5);
    m.AddWriteOp(backend::MutationOpType::kInsert, "Entries",
                 {"Id", "Seq", "Amount", "Day"},
                 {{ids[0], Int64(1), googlesql::values::Double(1.5),
                   googlesql::values::Date(20000)},
                  {ids[0], Int64(2), googlesql::values::Double(-2.5),
                   googlesql::values::NullDate()},
                  {ids[1], Int64(1), googlesql::values::Double(3),
                   googlesql::values::Date(1)}});
    m.AddWriteOp(
        backend::MutationOpType::kInsert, "Transfers",
        {"TransferId", "FromId", "Raw"},
        {{String("t1"), ids[2], googlesql::values::Bytes("\x01\x02")}});
    m.AddWriteOp(backend::MutationOpType::kUpdate, "Accounts",
                 {"Id", "Meta", "Tags"},
                 {{ids[3],
                   googlesql::values::Json(
                       googlesql::JSONValue::ParseJSONString(R"({"a": [1, 2]})")
                           .value()),
                   googlesql::values::StringArray({"x", "y"})}});
    GOOGLESQL_ASSERT_OK(Commit(db.get(), m).status());
    backend::Mutation del;
    backend::KeySet keys;
    keys.AddKey(backend::Key({ids[1]}));
    del.AddDeleteOp("Accounts", keys);
    GOOGLESQL_ASSERT_OK(Commit(db.get(), del).status());
  }
  // Schema changes with backfills, and a dropped table.
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      db.get(),
      {"ALTER TABLE Accounts ADD COLUMN Active BOOL NOT NULL DEFAULT (TRUE)",
       "CREATE INDEX EntriesByAmount ON Entries(Amount)",
       "CREATE TABLE Scratch (K INT64 NOT NULL) PRIMARY KEY (K)"}));
  backend::Mutation scratch;
  scratch.AddWriteOp(backend::MutationOpType::kInsert, "Scratch", {"K"},
                     {{Int64(1)}});
  GOOGLESQL_ASSERT_OK(Commit(db.get(), scratch).status());
  GOOGLESQL_ASSERT_OK(UpdateSchema(db.get(), {"DROP TABLE Scratch"}));

  const auto ddl = Ddl(db.get());
  const auto storage = DumpStorage(db.get());
  const auto rows = ReadTable(db.get(), "Accounts");
  const auto entries = ReadTable(db.get(), "Entries");
  const auto ids =
      backend::CollectSchemaIds(db->backend()->GetLatestSchemaShared().get());
  db.reset();
  Crash(emulator);

  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  EXPECT_EQ(Ddl(db.get()), ddl);
  EXPECT_EQ(
      backend::CollectSchemaIds(db->backend()->GetLatestSchemaShared().get()),
      ids);
  EXPECT_EQ(DumpStorage(db.get()), storage);
  EXPECT_EQ(ReadTable(db.get(), "Accounts"), rows);
  EXPECT_EQ(ReadTable(db.get(), "Entries"), entries);

  // Constraints still hold after recovery.
  EXPECT_FALSE(Commit(db.get(), InsertAccount("n0", 1)).ok());    // unique
  EXPECT_FALSE(Commit(db.get(), InsertAccount("neg", -1)).ok());  // check
  backend::Mutation dangling;
  dangling.AddWriteOp(backend::MutationOpType::kInsert, "Transfers",
                      {"TransferId", "FromId"}, {{String("t2"), Int64(42)}});
  EXPECT_FALSE(Commit(db.get(), dangling).ok());  // foreign key
  GOOGLESQL_EXPECT_OK(Commit(db.get(), InsertAccount("n9", 9)).status());

  // Checkpointed state recovers the same way, and new objects get fresh IDs.
  GOOGLESQL_ASSERT_OK(emulator->persistence->Checkpoint());
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      db.get(), {"CREATE TABLE Scratch (K INT64 NOT NULL) PRIMARY KEY (K)"}));
  const auto after_ddl = Ddl(db.get());
  const auto after_storage = DumpStorage(db.get());
  const std::string scratch_id =
      db->backend()->GetLatestSchema()->FindTable("Scratch")->id();
  EXPECT_FALSE(ids.tables.contains("Scratch"));
  db.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  EXPECT_EQ(Ddl(db.get()), after_ddl);
  EXPECT_EQ(DumpStorage(db.get()), after_storage);
  EXPECT_EQ(db->backend()->GetLatestSchema()->FindTable("Scratch")->id(),
            scratch_id);
}

TEST_F(PersistenceTest, SequenceValuesAreNeverHandedOutTwice) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  int inserted = 0;
  auto take = [&](int n) {
    for (int i = 0; i < n; ++i) {
      GOOGLESQL_ASSERT_OK(
          Commit(db.get(), InsertAccount(absl::StrCat("a", inserted++), 0))
              .status());
    }
  };
  int round = 0;
  for (; round < 4; ++round) {
    take(7);
    // An aborted use of the sequence still consumes its value.
    auto txn = db->backend()->CreateReadWriteTransaction(
        backend::ReadWriteOptions(), backend::RetryState());
    GOOGLESQL_ASSERT_OK(txn.status());
    GOOGLESQL_ASSERT_OK((*txn)->Write(InsertAccount("rolled-back", 0)));
    GOOGLESQL_ASSERT_OK((*txn)->Rollback());
    if (round == 2) GOOGLESQL_ASSERT_OK(emulator->persistence->Checkpoint());
    db.reset();
    Crash(emulator);
    emulator = StartOrDie();
    db = GetDatabase(emulator.get());
  }
  auto rows = ReadTable(db.get(), "Accounts");
  absl::flat_hash_set<std::string> distinct;
  for (const auto& row : rows) distinct.insert(row.substr(0, row.find(',')));
  EXPECT_EQ(rows.size(), 28);
  EXPECT_EQ(distinct.size(), rows.size());
}

TEST_F(PersistenceTest, ReadsFromBeforeTheRestartAreRefused) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time committed,
                                 Commit(db.get(), InsertAccount("a", 1)));
  db.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  backend::ReadOnlyOptions exact{
      .bound = backend::TimestampBound::kExactTimestamp,
      .timestamp = committed};
  EXPECT_THAT(db->backend()->CreateReadOnlyTransaction(exact).status(),
              StatusIs(absl::StatusCode::kFailedPrecondition));
  exact.timestamp = emulator->persistence->restart_floor();
  GOOGLESQL_EXPECT_OK(db->backend()->CreateReadOnlyTransaction(exact).status());
  // Bounded staleness picks a timestamp after the restart rather than fail.
  backend::ReadOnlyOptions bounded{
      .bound = backend::TimestampBound::kMaxStaleness,
      .staleness = absl::Hours(1)};
  for (int i = 0; i < 20; ++i) {
    auto txn = db->backend()->CreateReadOnlyTransaction(bounded);
    GOOGLESQL_ASSERT_OK(txn.status());
    EXPECT_GE((*txn)->read_timestamp(), emulator->persistence->restart_floor());
  }
  EXPECT_EQ(ReadTable(db.get(), "Accounts").size(), 1);
}

TEST_F(PersistenceTest, TimestampsAfterARestartExceedEveryEarlierOne) {
  // The system clock steps back an hour across the restart; the durable
  // lease still keeps every new timestamp above everything handed out.
  absl::Mutex mu;
  absl::Duration offset = absl::ZeroDuration();
  auto system_now = [&]() {
    absl::MutexLock lock(mu);
    return absl::Now() + offset;
  };
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(auto emulator, Start(system_now));
  auto db = CreateDatabase(emulator.get());
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time committed,
                                 Commit(db.get(), InsertAccount("a", 1)));
  absl::Time served = emulator->clock->Now();
  for (int i = 0; i < 1000; ++i) served = emulator->clock->Now();
  EXPECT_LT(committed, served);
  db.reset();
  Crash(emulator);
  {
    absl::MutexLock lock(mu);
    offset = -absl::Hours(1);
  }
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(emulator, Start(system_now));
  db = GetDatabase(emulator.get());
  EXPECT_GT(emulator->clock->Now(), served);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time after,
                                 Commit(db.get(), InsertAccount("b", 1)));
  EXPECT_GT(after, served);
}

TEST_F(PersistenceTest, DroppedDatabasesAndInstancesStayDropped) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("old", 1)).status());
  db.reset();
  GOOGLESQL_ASSERT_OK(emulator->databases->DeleteDatabase(kDatabase));
  // Same name, new incarnation and schema.
  db =
      CreateDatabase(emulator.get(), kDatabase,
                     {"CREATE TABLE Other (K INT64 NOT NULL) PRIMARY KEY (K)"});
  const auto ddl = Ddl(db.get());
  auto other =
      CreateDatabase(emulator.get(), "projects/p/instances/i1/databases/gone");
  other.reset();
  GOOGLESQL_ASSERT_OK(emulator->databases->DeleteDatabase(
      "projects/p/instances/i1/databases/gone"));
  db.reset();
  Crash(emulator);

  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  EXPECT_EQ(Ddl(db.get()), ddl);
  EXPECT_TRUE(ReadTable(db.get(), "Other").empty());
  EXPECT_FALSE(
      emulator->databases->GetDatabase("projects/p/instances/i1/databases/gone")
          .ok());
  GOOGLESQL_ASSERT_OK(emulator->persistence->Checkpoint());
  db.reset();
  GOOGLESQL_ASSERT_OK(emulator->databases->DeleteDatabase(kDatabase));
  GOOGLESQL_ASSERT_OK(emulator->instances->DeleteInstance(kInstance));
  Crash(emulator);
  emulator = StartOrDie();
  EXPECT_FALSE(emulator->databases->GetDatabase(kDatabase).ok());
  EXPECT_FALSE(emulator->instances->GetInstance(kInstance).ok());
}

TEST_F(PersistenceTest, PostgreSQLDatabasesAreRefused) {
  auto emulator = StartOrDie();
  CreateDatabase(emulator.get(), "projects/p/instances/i1/databases/unused",
                 {});
  EXPECT_THAT(
      emulator->databases->CreateDatabase(
          kDatabase,
          backend::SchemaChangeOperation{
              .database_dialect = database_api::DatabaseDialect::POSTGRESQL}),
      StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(PersistenceTest, FailedSyncFailsTheCommitAndEveryLaterOne) {
  auto emulator = StartOrDie(FrozenSystemClock);
  auto db = CreateDatabase(emulator.get());
  GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("durable", 1)).status());
  FailOn(Op::kSync);
  EXPECT_THAT(Commit(db.get(), InsertAccount("lost", 1)).status(),
              StatusIs(absl::StatusCode::kUnavailable));
  // Not acknowledged, and not applied either.
  EXPECT_EQ(ReadTable(db.get(), "Accounts").size(), 1);
  fs_.SetHook(nullptr);
  EXPECT_FALSE(Commit(db.get(), InsertAccount("later", 1)).ok());
  EXPECT_FALSE(
      UpdateSchema(db.get(), {"CREATE TABLE X (K INT64) PRIMARY KEY (K)"})
          .ok());
  EXPECT_FALSE(emulator->persistence->Checkpoint().ok());
  db.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  auto rows = ReadTable(db.get(), "Accounts");
  ASSERT_EQ(rows.size(), 1);
  EXPECT_THAT(rows[0], ::testing::HasSubstr("durable"));
}

// One checkpoint stage: the operation that fails and the file it acts on.
struct CheckpointStage {
  std::string name;
  Op op;
  std::string path;  // substring of the path
  // Whether the checkpoint is published (renamed into place and the
  // directory synced) before the stage fails.
  bool published;
};

TEST_F(PersistenceTest, CrashAtAnyCheckpointStageKeepsAcknowledgedData) {
  const std::vector<CheckpointStage> stages = {
      {"segment open", Op::kOpen, "/wal-", false},
      {"segment header write", Op::kAppend, "/wal-", false},
      {"segment sync", Op::kSync, "/wal-", false},
      {"segment directory sync", Op::kSyncDir, "/data", false},
      {"temporary file open", Op::kOpen, "checkpoint.tmp", false},
      {"temporary file write", Op::kAppend, "checkpoint.tmp", false},
      {"temporary file sync", Op::kSync, "checkpoint.tmp", false},
      {"rename", Op::kRename, "checkpoint.tmp", false},
      {"publishing directory sync", Op::kSyncDir, "/data", false},
      {"truncation", Op::kRemove, "/wal-", true},
  };
  for (const CheckpointStage& stage : stages) {
    SCOPED_TRACE(stage.name);
    fs_.SetHook(nullptr);
    fs_.Crash();
    auto names = fs_.ListDir("/data");
    GOOGLESQL_ASSERT_OK(names.status());
    for (const std::string& name : *names) {
      if (name != "LOCK") {
        GOOGLESQL_ASSERT_OK(fs_.Remove(absl::StrCat("/data/", name)));
      }
    }
    GOOGLESQL_ASSERT_OK(fs_.SyncDir("/data"));
    auto emulator = StartOrDie(FrozenSystemClock);
    auto db = CreateDatabase(emulator.get());
    GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("before", 1)).status());
    GOOGLESQL_ASSERT_OK(emulator->persistence->Checkpoint());
    GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("between", 2)).status());

    // Fail the first operation of the stage; the publishing directory sync
    // is the one after the rename.
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto renamed = std::make_shared<std::atomic<bool>>(false);
    fs_.SetHook([stage, fired, renamed](Op op, const std::string& path) {
      if (op == Op::kRename) *renamed = true;
      bool matches = op == stage.op && absl::StrContains(path, stage.path) &&
                     !*fired &&
                     (stage.name != "publishing directory sync" || *renamed);
      if (matches) {
        *fired = true;
        return absl::InternalError("injected I/O error");
      }
      return absl::OkStatus();
    });
    EXPECT_FALSE(emulator->persistence->Checkpoint().ok());
    fs_.SetHook(nullptr);
    EXPECT_TRUE(*fired) << "the checkpoint never reached " << stage.name;
    GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("after", 3)).status());
    const auto storage = DumpStorage(db.get());
    db.reset();
    Crash(emulator);
    emulator = StartOrDie();
    db = GetDatabase(emulator.get());
    EXPECT_EQ(DumpStorage(db.get()), storage);
    EXPECT_EQ(ReadTable(db.get(), "Accounts").size(), 3);
    db.reset();
    emulator.reset();
  }
}

TEST_F(PersistenceTest, AFailedSequenceDropKeepsItsState) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  for (int i = 0; i < 5; ++i) {
    GOOGLESQL_ASSERT_OK(
        Commit(db.get(), InsertAccount(absl::StrCat("a", i), 0)).status());
  }
  // Accounts.Id's default uses the sequence, so dropping it fails.
  EXPECT_FALSE(UpdateSchema(db.get(), {"DROP SEQUENCE seq"}).ok());
  for (int i = 5; i < 8; ++i) {
    GOOGLESQL_EXPECT_OK(
        Commit(db.get(), InsertAccount(absl::StrCat("a", i), 0)).status());
  }
  // The checkpoint covers the log that held the sequence's reservation.
  GOOGLESQL_ASSERT_OK(emulator->persistence->Checkpoint());
  db.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  for (int i = 8; i < 11; ++i) {
    GOOGLESQL_EXPECT_OK(
        Commit(db.get(), InsertAccount(absl::StrCat("a", i), 0)).status());
  }
  auto rows = ReadTable(db.get(), "Accounts");
  absl::flat_hash_set<std::string> ids;
  for (const auto& row : rows) ids.insert(row.substr(0, row.find(',')));
  EXPECT_EQ(rows.size(), 11);
  EXPECT_EQ(ids.size(), rows.size());
}

TEST_F(PersistenceTest, CheckpointFailsOnADatabaseItCannotReach) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  GOOGLESQL_ASSERT_OK(Commit(db.get(), InsertAccount("kept", 1)).status());
  db.reset();
  // The databases are gone while persistence still runs, as in a teardown
  // in the wrong order.
  emulator->databases.reset();
  EXPECT_FALSE(emulator->persistence->Checkpoint().ok());
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(ReadTable(db.get(), "Accounts").size(), 1);
}

TEST_F(PersistenceTest, TeardownStopsCheckpointsBeforeDroppingDatabases) {
  PersistenceOptions options;
  options.fs = &fs_;
  options.dir = "/data";
  options.checkpoint_bytes = 1;  // every record asks for a checkpoint
  auto env = std::make_unique<ServerEnv>();
  GOOGLESQL_ASSERT_OK(env->EnablePersistence(options));
  admin::instance::v1::Instance instance;
  instance.set_config("projects/p/instanceConfigs/emulator-config");
  instance.set_node_count(1);
  GOOGLESQL_ASSERT_OK(
      env->instance_manager()->CreateInstance(kInstance, instance).status());
  auto db = env->database_manager()->CreateDatabase(
      kDatabase, backend::SchemaChangeOperation{
                     .statements = kSchema,
                     .database_dialect =
                         database_api::DatabaseDialect::GOOGLE_STANDARD_SQL});
  GOOGLESQL_ASSERT_OK(db.status());
  std::weak_ptr<Database> weak = *db;

  // Hold the next background checkpoint right after it starts, until the
  // databases are destroyed (or two seconds pass), then let it capture.
  std::atomic<bool> armed = true;
  std::atomic<bool> entered = false;
  fs_.SetHook([&](Op op, const std::string& path) {
    if (op == Op::kOpen && absl::StrContains(path, "/wal-") &&
        armed.exchange(false)) {
      entered = true;
      for (int i = 0; i < 200 && !weak.expired(); ++i) {
        absl::SleepFor(absl::Milliseconds(10));
      }
    }
    return absl::OkStatus();
  });
  GOOGLESQL_ASSERT_OK(Commit(db->get(), InsertAccount("kept", 1)).status());
  db->reset();
  for (int i = 0; i < 1000 && !entered; ++i) {
    absl::SleepFor(absl::Milliseconds(10));
  }
  ASSERT_TRUE(entered);
  env.reset();
  fs_.SetHook(nullptr);
  fs_.Crash();

  auto emulator = StartOrDie();
  auto recovered = GetDatabase(emulator.get());
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(ReadTable(recovered.get(), "Accounts").size(), 1);
}

TEST_F(PersistenceTest, ExposedReadTimestampsAreCoveredAcrossARestart) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  // What BeginTransaction(read_only{read_timestamp: T,
  // return_read_timestamp: true}) returns to the client.
  const absl::Time future = emulator->clock->Now() + absl::Seconds(2);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      auto txn,
      db->backend()->CreateReadOnlyTransaction(backend::ReadOnlyOptions{
          .bound = backend::TimestampBound::kExactTimestamp,
          .timestamp = future}));
  EXPECT_EQ(txn->read_timestamp(), future);
  txn.reset();
  db.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  EXPECT_GT(emulator->persistence->restart_floor(), future);
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(absl::Time after,
                                 Commit(db.get(), InsertAccount("b", 1)));
  EXPECT_GT(after, future);
}

TEST_F(PersistenceTest, SchemaChangesAreInvisibleUntilTheirRecordIsSynced) {
  // A frozen system clock: no lease extension shares the blocked sync.
  auto emulator = StartOrDie(FrozenSystemClock);
  auto db = CreateDatabase(emulator.get());
  absl::Mutex mu;
  bool armed = true, blocked = false, released = false;
  fs_.SetHook([&](Op op, const std::string& path) {
    if (op != Op::kSync || !absl::StrContains(path, "/wal-")) {
      return absl::OkStatus();
    }
    absl::MutexLock lock(mu);
    if (!armed) return absl::OkStatus();
    armed = false;
    blocked = true;
    mu.AwaitWithTimeout(absl::Condition(&released), absl::Seconds(10));
    return absl::OkStatus();
  });
  std::thread ddl([&] {
    GOOGLESQL_EXPECT_OK(UpdateSchema(
        db.get(), {"CREATE TABLE Later (K INT64 NOT NULL) PRIMARY KEY (K)"}));
  });
  {
    absl::MutexLock lock(mu);
    ASSERT_TRUE(
        mu.AwaitWithTimeout(absl::Condition(&blocked), absl::Seconds(10)));
  }
  // What GetDatabaseDdl, queries and new transactions would see.
  EXPECT_EQ(db->backend()->GetLatestSchemaShared()->FindTable("Later"),
            nullptr);
  EXPECT_EQ(db->backend()->GetLatestSchema()->FindTable("Later"), nullptr);
  for (const std::string& statement : Ddl(db.get())) {
    EXPECT_THAT(statement, ::testing::Not(::testing::HasSubstr("Later")));
  }
  {
    absl::MutexLock lock(mu);
    released = true;
  }
  ddl.join();
  fs_.SetHook(nullptr);
  EXPECT_NE(db->backend()->GetLatestSchemaShared()->FindTable("Later"),
            nullptr);
}

TEST_F(PersistenceTest, RecoveryIgnoresValuesOfDroppedProtoAndEnumColumns) {
  google::protobuf::FileDescriptorProto file;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(
      R"pb(
        syntax: "proto2"
        name: "user.proto"
        package: "customer.app"
        message_type {
          name: "User"
          field {
            name: "name"
            number: 1
            label: LABEL_OPTIONAL
            type: TYPE_STRING
          }
        }
        enum_type {
          name: "State"
          value { name: "UNSPECIFIED" number: 0 }
          value { name: "ACTIVE" number: 1 }
        }
      )pb",
      &file));
  google::protobuf::FileDescriptorSet files;
  *files.add_file() = file;
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get(), kDatabase, {});
  int successful;
  absl::Time timestamp;
  absl::Status backfill;
  GOOGLESQL_ASSERT_OK(db->backend()->UpdateSchema(
      backend::SchemaChangeOperation{
          .statements =
              {"CREATE PROTO BUNDLE (customer.app.User, "
               "customer.app.State)",
               "CREATE TABLE P (K INT64 NOT NULL, U customer.app.User, "
               "S customer.app.State) PRIMARY KEY (K)"},
          .proto_descriptor_bytes = files.SerializeAsString(),
          .database_dialect =
              database_api::DatabaseDialect::GOOGLE_STANDARD_SQL},
      &successful, &timestamp, &backfill));
  GOOGLESQL_ASSERT_OK(backfill);
  const backend::Table* table =
      db->backend()->GetLatestSchema()->FindTable("P");
  const googlesql::Type* user = table->FindColumn("U")->GetType();
  const googlesql::Type* state = table->FindColumn("S")->GetType();
  backend::Mutation m;
  m.AddWriteOp(backend::MutationOpType::kInsert, "P", {"K", "U", "S"},
               {{Int64(1),
                 googlesql::Value::Proto(user->AsProto(), absl::Cord("\x0a\x03"
                                                                     "ann")),
                 googlesql::Value::Enum(state->AsEnum(), 1)}});
  GOOGLESQL_ASSERT_OK(Commit(db.get(), m).status());
  GOOGLESQL_ASSERT_OK(UpdateSchema(
      db.get(), {"ALTER TABLE P DROP COLUMN U", "ALTER TABLE P DROP COLUMN S",
                 "DROP PROTO BUNDLE"}));
  const auto storage = DumpStorage(db.get());
  db.reset();
  // No checkpoint: recovery replays the log, whose insert still holds the
  // values of the dropped proto and enum types.
  Crash(emulator);
  emulator = StartOrDie();
  db = GetDatabase(emulator.get());
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(DumpStorage(db.get()), storage);
  EXPECT_EQ(ReadTable(db.get(), "P").size(), 1);
}

TEST_F(PersistenceTest, ConcurrentCommitsCheckpointsAndACrash) {
  auto emulator = StartOrDie();
  auto db1 = CreateDatabase(emulator.get());
  auto db2 =
      CreateDatabase(emulator.get(), "projects/p/instances/i1/databases/db2");
  constexpr int kThreads = 8;
  constexpr int kCommits = 40;
  std::atomic<bool> stop = false;
  std::thread checkpointer([&] {
    while (!stop) {
      GOOGLESQL_EXPECT_OK(emulator->persistence->Checkpoint());
      absl::SleepFor(absl::Milliseconds(2));
    }
  });
  std::vector<std::thread> threads;
  std::atomic<int> committed = 0;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Database* db = t % 2 == 0 ? db1.get() : db2.get();
      for (int i = 0; i < kCommits; ++i) {
        // Sequence defaults, reads and clock leases all race with the
        // checkpoints and with each other; aborts are retried.
        while (!Commit(db, InsertAccount(absl::StrCat(t, "-", i), i)).ok()) {
        }
        ++committed;
        if (i % 10 == 0) {
          ReadTable(db, "Accounts");
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  stop = true;
  checkpointer.join();
  EXPECT_EQ(committed, kThreads * kCommits);
  const auto storage1 = DumpStorage(db1.get());
  const auto storage2 = DumpStorage(db2.get());
  db1.reset();
  db2.reset();
  Crash(emulator);
  emulator = StartOrDie();
  db1 = GetDatabase(emulator.get());
  db2 = GetDatabase(emulator.get(), "projects/p/instances/i1/databases/db2");
  EXPECT_EQ(DumpStorage(db1.get()), storage1);
  EXPECT_EQ(DumpStorage(db2.get()), storage2);
  EXPECT_EQ(ReadTable(db1.get(), "Accounts").size(), kThreads / 2 * kCommits);
  EXPECT_EQ(ReadTable(db2.get(), "Accounts").size(), kThreads / 2 * kCommits);
}

TEST_F(PersistenceTest, CorruptLogRefusesToStart) {
  auto emulator = StartOrDie();
  auto db = CreateDatabase(emulator.get());
  for (int i = 0; i < 3; ++i) {
    GOOGLESQL_ASSERT_OK(
        Commit(db.get(), InsertAccount(absl::StrCat("n", i), i)).status());
  }
  db.reset();
  Crash(emulator);
  const std::string segment = "/data/wal-00000000000000000000.log";
  std::string data = *fs_.Contents(segment);
  data[data.size() / 2] ^= 0x10;
  fs_.Overwrite(segment, data);
  EXPECT_THAT(Start().status(), StatusIs(absl::StatusCode::kDataLoss));
}

}  // namespace
}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
