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

#include "backend/database/database.h"

#include <memory>
#include <optional>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/functional/bind_front.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "backend/actions/manager.h"
#include "backend/common/ids.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/pg_oid_assigner/pg_oid_assigner.h"
#include "backend/locking/manager.h"
#include "backend/query/query_engine.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/schema/updater/scoped_schema_change_lock.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/storage/recording_storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/errors.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

void AddTableIds(const Table* table, SchemaIds* ids) {
  if (table == nullptr) return;
  ids->tables[table->Name()] = table->id();
  for (const Column* column : table->columns()) {
    ids->columns[absl::StrCat(table->Name(), ".", column->Name())] =
        column->id();
  }
}

// Names present in exactly one of `a` and `b`, or mapped differently.
template <typename Map>
std::vector<std::string> Differences(const Map& a, const Map& b) {
  std::vector<std::string> names;
  for (const auto& [name, id] : a) {
    auto it = b.find(name);
    if (it == b.end() || it->second != id) names.push_back(name);
  }
  for (const auto& [name, id] : b) {
    if (!a.contains(name)) names.push_back(name);
  }
  return names;
}

}  // namespace

SchemaIds CollectSchemaIds(const Schema* schema) {
  SchemaIds ids;
  for (const Table* table : schema->tables()) {
    AddTableIds(table, &ids);
    for (const Index* index : table->indexes()) {
      AddTableIds(index->index_data_table(), &ids);
    }
  }
  for (const ChangeStream* change_stream : schema->change_streams()) {
    AddTableIds(change_stream->change_stream_data_table(), &ids);
    AddTableIds(change_stream->change_stream_partition_table(), &ids);
  }
  for (const Sequence* sequence : schema->sequences()) {
    ids.sequences[sequence->Name()] = sequence->id();
  }
  return ids;
}

// TransactionIDGenerator is initialized to 1 because 0 is used as a sentinel
// value for an invalid transaction.
Database::Database()
    : transaction_id_generator_(absl::ToUnixMicros(absl::Now())) {}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const SchemaChangeOperation& schema_change_operation,
    std::unique_ptr<DatabaseLog> log, const DatabaseRestore* restore) {
  auto database = absl::WrapUnique(new Database());
  database->clock_ = clock;
  database->database_id_ = database_id;
  database->log_ = std::move(log);
  database->storage_ = std::make_unique<InMemoryStorage>();
  database->lock_manager_ =
      std::make_unique<LockManager>(clock, database->storage_.get());
  if (database->log_ != nullptr) {
    database->lock_manager_->set_commit_gate(database->log_->commit_gate());
  }
  database->type_factory_ = std::make_unique<googlesql::TypeFactory>();
  database->action_manager_ = std::make_unique<ActionManager>();
  database->dialect_ = schema_change_operation.database_dialect;
  database->pg_oid_assigner_ = std::make_unique<PgOidAssigner>(
      schema_change_operation.database_dialect ==
      database_api::DatabaseDialect::POSTGRESQL);

  if (schema_change_operation.statements.empty()) {
    if (database->dialect_ == database_api::DatabaseDialect::POSTGRESQL) {
      // Create an empty schema with the dialect set.
      database->versioned_catalog_ =
          std::make_unique<VersionedCatalog>(std::make_unique<const Schema>(
              SchemaGraph::CreateEmpty(), ProtoBundle::CreateEmpty(),
              database->dialect_, database_id));
    } else {
      database->versioned_catalog_ = std::make_unique<VersionedCatalog>();
    }
  } else {
    SchemaChangeContext context = database->GetSchemaChangeContext();
    if (restore != nullptr) {
      database->table_id_generator_.Preassign(restore->ids.tables);
      database->column_id_generator_.Preassign(restore->ids.columns);
      context.sequence_ids = &restore->ids.sequences;
    }
    SchemaUpdater updater;
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const Schema> schema,
        updater.CreateSchemaFromDDL(schema_change_operation, context));
    if (restore != nullptr) {
      database->table_id_generator_.TakeUnusedPreassigned();
      database->column_id_generator_.TakeUnusedPreassigned();
      SchemaIds rebuilt = CollectSchemaIds(schema.get());
      if (rebuilt != restore->ids) {
        std::vector<std::string> names =
            Differences(rebuilt.tables, restore->ids.tables);
        for (auto& name : Differences(rebuilt.columns, restore->ids.columns)) {
          names.push_back(std::move(name));
        }
        for (auto& name :
             Differences(rebuilt.sequences, restore->ids.sequences)) {
          names.push_back(std::move(name));
        }
        return absl::DataLossError(absl::StrCat(
            "Rebuilding the schema of database ", database_id,
            " from its persisted DDL gives different storage IDs for: ",
            absl::StrJoin(names, ", ")));
      }
    }
    database->versioned_catalog_ =
        std::make_unique<VersionedCatalog>(std::move(schema));
  }
  if (restore != nullptr) {
    database->table_id_generator_.set_next_seq(restore->next_table_seq);
    database->column_id_generator_.set_next_seq(restore->next_column_seq);
  }

  database->query_engine_ = std::make_unique<QueryEngine>(
      database->type_factory_.get(),
      database->versioned_catalog_->GetLatestSchema());

  database->action_manager_->AddActionsForSchema(
      database->versioned_catalog_->GetLatestSchema(),
      database->query_engine_->function_catalog(),
      database->query_engine_->type_factory());

  database->change_stream_partition_churner_ =
      std::make_unique<ChangeStreamPartitionChurner>(
          absl::bind_front(&Database::CreateReadWriteTransaction,
                           database.get()),
          database->clock_);

  database->change_stream_partition_churner_->Update(
      database->versioned_catalog_->GetLatestSchema());

  // Some functions need to access the schema (e.g. sequence functions), so
  // set the latest schema to the function catalog here.
  database->query_engine_->SetLatestSchemaForFunctionCatalog(
      database->versioned_catalog_->GetLatestSchemaShared());

  database->storage_->SetVersionRetentionPeriod(
      database->versioned_catalog_->version_retention_period());

  return database;
}
absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>
Database::CreateReadOnlyTransaction(const ReadOnlyOptions& options) {
  auto transaction = std::make_unique<ReadOnlyTransaction>(
      options, transaction_id_generator_.NextId(), clock_, storage_.get(),
      lock_manager_.get(), versioned_catalog_.get());
  if (transaction->read_timestamp() < restart_floor_) {
    return error::ReadTimestampBeforeRestart(transaction->read_timestamp(),
                                             restart_floor_);
  }
  if (log_ != nullptr) {
    // A caller-chosen timestamp (exact or minimum bound) can be past the
    // clock's lease, and BeginTransaction returns it. Cover it durably, so
    // that after a crash the clock resumes above it.
    GOOGLESQL_RETURN_IF_ERROR(
        clock_->CoverWithLease(transaction->read_timestamp()));
  }
  return transaction;
}

PersistedSchema Database::GetPersistedSchema() {
  return PersistedSchema{
      .schema = versioned_catalog_->GetLatestSchemaShared(),
      .next_table_seq = table_id_generator_.next_seq(),
      .next_column_seq = column_id_generator_.next_seq(),
  };
}

void Database::SetRestartFloor(absl::Time floor) {
  restart_floor_ = floor;
  lock_manager_->AdvanceLastCommitTimestamp(floor);
}

absl::StatusOr<std::unique_ptr<ReadWriteTransaction>>
Database::CreateReadWriteTransaction(const ReadWriteOptions& options,
                                     const RetryState& retry_state) {
  return std::make_unique<ReadWriteTransaction>(
      options, retry_state, transaction_id_generator_.NextId(), clock_,
      storage_.get(), lock_manager_.get(), versioned_catalog_.get(),
      action_manager_.get(), log_.get());
}

SchemaChangeContext Database::GetSchemaChangeContext() {
  return SchemaChangeContext{
      .type_factory = type_factory_.get(),
      .table_id_generator = &table_id_generator_,
      .column_id_generator = &column_id_generator_,
      .storage = storage_.get(),
      .pg_oid_assigner = pg_oid_assigner_.get(),
      .database_id = database_id_,
  };
}

absl::Status Database::UpdateSchema(
    const SchemaChangeOperation& schema_change_operation,
    int* num_succesful_statements, absl::Time* commit_timestamp,
    absl::Status* backfill_status) {
  if (schema_change_operation.statements.empty()) {
    return error::UpdateDatabaseMissingStatements();
  }

  // One schema change at a time, so that churners are reconciled with the
  // schemas in commit order.
  absl::MutexLock schema_change_lock(schema_change_mu_);
  {
    // Hold the commit critical section exclusively: in-flight commits finish
    // first, and open read-write transactions abort once they see the new
    // schema. The change stream churner is updated after the lock is released,
    // because stopping a churning thread joins it, and that thread may be
    // waiting to commit.
    ScopedSchemaChangeLock lock{transaction_id_generator_.NextId(),
                                lock_manager_.get()};
    GOOGLESQL_RETURN_IF_ERROR(lock.Wait());
    GOOGLESQL_RETURN_IF_ERROR(ApplySchemaChangeLocked(
        schema_change_operation, lock, num_succesful_statements,
        commit_timestamp, backfill_status));
  }
  const Schema* schema = versioned_catalog_->GetLatestSchema();
  if (before_churner_update_hook_ != nullptr) {
    before_churner_update_hook_();
  }
  change_stream_partition_churner_->Update(schema);
  return absl::OkStatus();
}

absl::Status Database::ApplySchemaChangeLocked(
    const SchemaChangeOperation& schema_change_operation,
    ScopedSchemaChangeLock& lock, int* num_succesful_statements,
    absl::Time* commit_timestamp, absl::Status* backfill_status) {
  // With --data_dir, the commit gate is held from reserving the timestamp
  // until the change is logged, and backfill writes are recorded for the log.
  std::optional<absl::MutexLock> gate;
  std::optional<RecordingStorage> recorded;
  if (log_ != nullptr) {
    gate.emplace(*log_->commit_gate());
    GOOGLESQL_RETURN_IF_ERROR(log_->health());
    recorded.emplace(storage_.get(), /*forward=*/true);
  }

  // Reserve a commit timestamp for the schema changes. Even if the
  // schema change fails, it will result in a no-op commit that will
  // be invisible to other read-only/read-write transactions.
  GOOGLESQL_ASSIGN_OR_RETURN(auto update_timestamp, lock.ReserveCommitTimestamp());

  auto context = GetSchemaChangeContext();
  context.schema_change_timestamp = update_timestamp;
  if (recorded.has_value()) context.storage = &*recorded;
  // Owned: the sequences it drops are cleaned up after it is replaced.
  std::shared_ptr<const Schema> existing_schema =
      versioned_catalog_->GetLatestSchemaShared();
  std::vector<std::string> created_sequence_ids;
  context.created_sequence_ids = &created_sequence_ids;
  // Whatever the outcome, forget the counters of sequences missing from the
  // published schema: dropped ones, and ones this change created but that
  // did not survive it. A DROP SEQUENCE that fails keeps its state.
  absl::Cleanup forget_sequences = [&] {
    std::shared_ptr<const Schema> latest =
        versioned_catalog_->GetLatestSchemaShared();
    absl::flat_hash_set<std::string> live;
    for (const Sequence* sequence : latest->sequences()) {
      live.insert(sequence->id());
    }
    for (const Sequence* sequence : existing_schema->sequences()) {
      if (!live.contains(sequence->id())) Sequence::ForgetState(sequence->id());
    }
    for (const std::string& id : created_sequence_ids) {
      if (!live.contains(id)) Sequence::ForgetState(id);
    }
  };
  SchemaUpdater updater;
  auto updated = updater.UpdateSchemaFromDDL(existing_schema.get(),
                                             schema_change_operation, context);
  if (!updated.ok()) {
    // Statements validated before the failing one may have marked tables and
    // columns dropped.
    storage_->RollBackVersionsAt(update_timestamp);
    return updated.status();
  }
  SchemaChangeResult result = *std::move(updated);
  *commit_timestamp = update_timestamp;
  *num_succesful_statements = result.num_successful_statements;
  *backfill_status = result.backfill_status;

  // Every check AddSchema makes runs here, before the record is written, so
  // that installing a logged schema cannot fail. A rejected change is undone
  // in storage: its backfills may have rewritten surviving columns (a type
  // change) and its drops marked tables for cleanup. Upstream kept both,
  // leaving values of the new type under the old schema.
  if (result.updated_schema != nullptr) {
    absl::Status rejected = versioned_catalog_->CheckSchema(
        update_timestamp, *result.updated_schema);
    if (!rejected.ok()) {
      storage_->RollBackVersionsAt(update_timestamp);
      return rejected;
    }
  }

  // With --data_dir, the new schema is published only once its record is
  // synced: nothing (GetDatabaseDdl, queries, new transactions) can see it
  // before. Its backfill writes are already in storage but invisible, since
  // reads at or after its timestamp wait until it is marked committed. If the
  // record fails the emulator stops; restarting recovers what the log holds.
  if (log_ != nullptr &&
      (result.updated_schema != nullptr || !recorded->ops().empty())) {
    PersistedSchema persisted = GetPersistedSchema();
    if (result.updated_schema != nullptr) {
      // Borrowed for the call; `result` owns it.
      persisted.schema = std::shared_ptr<const Schema>(
          std::shared_ptr<const Schema>(), result.updated_schema.get());
    }
    absl::Status status =
        log_->LogSchemaChange(update_timestamp, persisted, recorded->ops());
    if (!status.ok()) {
      std::fprintf(stderr, "Cannot log a schema change of database %s: %s\n",
                   database_id_.c_str(), status.ToString().c_str());
      std::abort();
    }
  }

  // We update the schema even if the backfill status was not OK, the returned
  // schema will be the schema for the last valid statement before the statement
  // for which the backfill/verification failed.
  if (result.updated_schema != nullptr) {
    absl::Status added = versioned_catalog_->AddSchema(
        update_timestamp, std::move(result.updated_schema));
    if (!added.ok()) {
      if (log_ != nullptr) {
        // Checked above, so unreachable; the log already holds the schema.
        std::fprintf(stderr, "Cannot install a logged schema of %s: %s\n",
                     database_id_.c_str(), added.ToString().c_str());
        std::abort();
      }
      return added;
    }
    action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                         query_engine_->function_catalog(),
                                         query_engine_->type_factory());
  }
  // Some functions need to access the schema (e.g. sequence functions), so
  // set the latest schema to the function catalog here.
  query_engine_->SetLatestSchemaForFunctionCatalog(
      versioned_catalog_->GetLatestSchemaShared());

  storage_->SetVersionRetentionPeriod(
      versioned_catalog_->version_retention_period());

  // Enforce the retention period.
  storage_->CleanUpDeletedTables(update_timestamp);
  storage_->CleanUpDeletedColumns(update_timestamp);
  versioned_catalog_->RemoveExpiredSchemas(update_timestamp);

  return absl::OkStatus();
}

const Schema* Database::GetLatestSchema() const {
  return versioned_catalog_->GetLatestSchema();
}

std::shared_ptr<const Schema> Database::GetLatestSchemaShared() const {
  return versioned_catalog_->GetLatestSchemaShared();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
