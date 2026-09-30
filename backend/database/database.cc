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

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "google/spanner/admin/database/v1/common.pb.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/bind_front.h"
#include "absl/log/log.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/variant.h"
#include "backend/actions/manager.h"
#include "backend/common/ids.h"
#include "backend/database/change_stream/change_stream_partition_churner.h"
#include "backend/database/pg_oid_assigner/pg_oid_assigner.h"
#include "backend/database/schema_create_cache.h"
#include "backend/locking/manager.h"
#include "backend/query/query_engine.h"
#include "backend/schema/backfills/change_stream_backfill.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/versioned_catalog.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/schema/updater/schema_validation_context.h"
#include "backend/schema/updater/scoped_schema_change_lock.h"
#include "backend/storage/in_memory_storage.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_only_transaction.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/errors.h"
#include "common/heap_release.h"
#include "googlesql/base/logging.h"
#include "googlesql/base/status_macros.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// TransactionIDGenerator is initialized to 1 because 0 is used as a sentinel
// value for an invalid transaction.
Database::Database()
    : transaction_id_generator_(absl::ToUnixMicros(absl::Now())) {
  static std::atomic<int64_t> next_database = 0;
  sequence_id_prefix_ = absl::StrCat("seq_db", next_database++, "_");
}

Database::~Database() {
  Sequence::RemoveSequenceCountersWithIdPrefix(sequence_id_prefix_);
}

namespace {

// The prefix of a cache entry's sequence ids. No database's prefix
// ("seq_db<N>_") is a prefix of it, or it of theirs. Nothing draws from an
// entry's sequences, so they never have counters.
constexpr char kCacheEntrySequenceIdPrefix[] = "seq_cache_";

}  // namespace

std::unique_ptr<Database> Database::New(
    Clock* clock, std::string_view database_id,
    database_api::DatabaseDialect dialect) {
  auto database = absl::WrapUnique(new Database());
  database->clock_ = clock;
  database->database_id_ = database_id;
  database->storage_ = std::make_unique<InMemoryStorage>();
  database->lock_manager_ =
      std::make_unique<LockManager>(clock, database->storage_.get());
  database->type_factory_ = std::make_unique<googlesql::TypeFactory>();
  database->action_manager_ = std::make_unique<ActionManager>();
  database->dialect_ = dialect;
  database->pg_oid_assigner_ = std::make_unique<PgOidAssigner>(
      dialect == database_api::DatabaseDialect::POSTGRESQL);
  return database;
}

absl::StatusOr<std::unique_ptr<Database>> Database::Create(
    Clock* clock, std::string_view database_id,
    const SchemaChangeOperation& schema_change_operation) {
  SchemaCreateCache* cache = schema_change_operation.statements.empty()
                                 ? nullptr
                                 : SchemaCreateCache::Global();
  SchemaCreateCache::Settings settings;
  std::string cache_key;
  std::shared_ptr<const SchemaCreateCache::Entry> cached;
  if (cache != nullptr) {
    settings = SchemaCreateCache::CurrentSettings();
    cache_key = SchemaCreateCache::Key(schema_change_operation, settings);
    cached = cache->Lookup(cache_key);
    SchemaCreateCache::RunAfterKeyHook();
  }
  if (cached != nullptr && cached->schema != nullptr) {
    absl::StatusOr<std::unique_ptr<Database>> database =
        CreateFromCache(clock, database_id,
                        schema_change_operation.database_dialect, *cached);
    if (database.ok()) {
      cache->RecordCopy();
      return database;
    }
    cache->RecordFallback();
    ABSL_LOG(WARNING) << "Creating database " << database_id
                      << " from DDL: cannot copy its cached schema: "
                      << database.status();
  }

  std::unique_ptr<Database> database =
      New(clock, database_id, schema_change_operation.database_dialect);
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
    SchemaUpdater updater;
    GOOGLESQL_ASSIGN_OR_RETURN(
        std::unique_ptr<const Schema> schema,
        updater.CreateSchemaFromDDL(schema_change_operation,
                                    database->GetSchemaChangeContext()));
    database->versioned_catalog_ =
        std::make_unique<VersionedCatalog>(std::move(schema));
    // An entry without a schema records that the schema cannot be copied.
    // All-no-op statements leave no schema to copy.
    if (cache != nullptr && cached == nullptr &&
        database->versioned_catalog_->GetLatestSchema() != nullptr) {
      std::shared_ptr<const SchemaCreateCache::Entry> entry =
          database->MakeSchemaCreateCacheEntry();
      SchemaCreateCache::RunBeforePublishHook();
      // The DDL may have read other settings than the key's.
      if (SchemaCreateCache::SettingsUnchanged(settings)) {
        cache->Insert(cache_key, std::move(entry));
      } else {
        cache->RecordRejected();
      }
    }
  }
  database->Initialize();
  return database;
}

absl::StatusOr<std::unique_ptr<Database>> Database::CreateFromCache(
    Clock* clock, std::string_view database_id,
    database_api::DatabaseDialect dialect,
    const SchemaCreateCache::Entry& entry) {
  std::unique_ptr<Database> database = New(clock, database_id, dialect);
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<const Schema> schema,
                   CopySchema(*entry.schema, database->type_factory_.get(),
                              database_id, database->sequence_id_prefix_,
                              absl::Now()));
  // Continue the counters from where creating the schema from DDL left them.
  database->table_id_generator_.set_next_seq(entry.next_table_id);
  database->column_id_generator_.set_next_seq(entry.next_column_id);
  if (entry.next_postgresql_oid.has_value()) {
    database->pg_oid_assigner_->SetNextPostgresqlOid(
        *entry.next_postgresql_oid);
  }
  // Creating a change stream from DDL backfills its initial partitions.
  SchemaChangeContext context = database->GetSchemaChangeContext();
  SchemaValidationContext validation_context(
      context.storage, /*global_names=*/nullptr, context.type_factory,
      context.schema_change_timestamp, dialect);
  for (const ChangeStream* change_stream : schema->change_streams()) {
    GOOGLESQL_RETURN_IF_ERROR(BackfillChangeStream(change_stream, &validation_context));
  }
  database->versioned_catalog_ =
      std::make_unique<VersionedCatalog>(std::move(schema));
  database->Initialize();
  return database;
}

absl::StatusOr<std::unique_ptr<Database>> Database::CreateEphemeralCopy() {
  // No schema change runs until the copy is done: none can drop or restart a
  // sequence between T and the copy of its counter, and the ID generators are
  // not moving. Copies only read, so they share the lock.
  absl::ReaderMutexLock schema_change_lock(schema_change_mu_);
  std::unique_ptr<Database> copy = New(clock_, database_id_, dialect_);
  const absl::Time timestamp = clock_->Now();
  if (ephemeral_copy_hook_ != nullptr) {
    ephemeral_copy_hook_(timestamp);
  }
  // Commits that reserved a timestamp at or before T have finished flushing;
  // later ones write versions after T.
  lock_manager_->WaitForSafeRead(timestamp);
  std::shared_ptr<const Schema> schema =
      versioned_catalog_->GetSchemaShared(timestamp);
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::unique_ptr<const Schema> schema_copy,
      CopySchema(*schema, copy->type_factory_.get(), database_id_,
                 copy->sequence_id_prefix_, /*now=*/std::nullopt));

  copy->table_id_generator_.set_next_seq(table_id_generator_.next_seq());
  copy->column_id_generator_.set_next_seq(column_id_generator_.next_seq());
  copy->change_stream_id_generator_.set_next_seq(
      change_stream_id_generator_.next_seq());
  if (std::optional<uint32_t> oid = pg_oid_assigner_->next_postgresql_oid();
      oid.has_value()) {
    copy->pg_oid_assigner_->SetNextPostgresqlOid(*oid);
  }

  // Index data and change stream tables are tables of the graph too.
  absl::flat_hash_map<TableID, std::vector<ColumnID>> columns;
  for (const SchemaNode* node : schema->GetSchemaGraph()->GetSchemaNodes()) {
    if (const Table* table = node->As<const Table>(); table != nullptr) {
      std::vector<ColumnID>& column_ids = columns[table->id()];
      for (const Column* column : table->columns()) {
        column_ids.push_back(column->id());
      }
    }
  }
  // Values take the copy's types, so they reference only the copy's
  // descriptors: the base may free its proto bundle while the copy lives.
  const ProtoBundle& proto_bundle = *schema_copy->proto_bundle();
  googlesql::TypeFactory* type_factory = copy->type_factory_.get();
  GOOGLESQL_RETURN_IF_ERROR(storage_->CopyAt(
      timestamp, columns,
      [&](const googlesql::Value& value) {
        return CopyValue(value, type_factory, proto_bundle);
      },
      copy->storage_.get()));

  // Counters are not versioned. Read after T, each is at least what the rows
  // at T drew from it.
  absl::flat_hash_map<std::string, SequenceID> sequence_ids;
  for (const Sequence* sequence : schema->sequences()) {
    sequence_ids[sequence->Name()] = sequence->id();
  }
  std::vector<std::pair<SequenceID, SequenceID>> counters;
  for (const Sequence* sequence : schema_copy->sequences()) {
    if (auto it = sequence_ids.find(sequence->Name());
        it != sequence_ids.end()) {
      counters.emplace_back(it->second, sequence->id());
    }
  }
  Sequence::CopySequenceCounters(counters);

  copy->lock_manager_->SeedLastCommitTimestamp(timestamp);
  copy->versioned_catalog_ = std::make_unique<VersionedCatalog>(
      std::move(schema_copy), versioned_catalog_->version_retention_period());
  copy->Initialize();
  return copy;
}

std::shared_ptr<const SchemaCreateCache::Entry>
Database::MakeSchemaCreateCacheEntry() const {
  auto entry = std::make_shared<SchemaCreateCache::Entry>();
  entry->type_factory = std::make_unique<googlesql::TypeFactory>();
  absl::StatusOr<std::unique_ptr<const Schema>> schema =
      CopySchema(*versioned_catalog_->GetLatestSchema(),
                 entry->type_factory.get(), /*database_id=*/"",
                 kCacheEntrySequenceIdPrefix, absl::Now());
  if (schema.ok()) {
    entry->schema = *std::move(schema);
  } else {
    GOOGLESQL_VLOG(1) << "Not caching the schema of database " << database_id_
            << ": " << schema.status();
  }
  entry->next_table_id = table_id_generator_.next_seq();
  entry->next_column_id = column_id_generator_.next_seq();
  entry->next_postgresql_oid = pg_oid_assigner_->next_postgresql_oid();
  return entry;
}

void Database::Initialize() {
  query_engine_ = std::make_unique<QueryEngine>(
      type_factory_.get(), versioned_catalog_->GetLatestSchema());

  action_manager_->AddActionsForSchema(versioned_catalog_->GetLatestSchema(),
                                       query_engine_->function_catalog(),
                                       query_engine_->type_factory());

  change_stream_partition_churner_ =
      std::make_unique<ChangeStreamPartitionChurner>(
          absl::bind_front(&Database::CreateReadWriteTransaction, this),
          clock_);

  change_stream_partition_churner_->Update(
      versioned_catalog_->GetLatestSchema());

  // Some functions need to access the schema (e.g. sequence functions), so
  // set the latest schema to the function catalog here.
  query_engine_->SetLatestSchemaForFunctionCatalog(
      versioned_catalog_->GetLatestSchemaShared());

  storage_->SetVersionRetentionPeriod(
      versioned_catalog_->version_retention_period());
}

absl::StatusOr<std::unique_ptr<ReadOnlyTransaction>>
Database::CreateReadOnlyTransaction(const ReadOnlyOptions& options) {
  return std::make_unique<ReadOnlyTransaction>(
      options, transaction_id_generator_.NextId(), clock_, storage_.get(),
      lock_manager_.get(), versioned_catalog_.get());
}

absl::StatusOr<std::unique_ptr<ReadWriteTransaction>>
Database::CreateReadWriteTransaction(const ReadWriteOptions& options,
                                     const RetryState& retry_state) {
  return std::make_unique<ReadWriteTransaction>(
      options, retry_state, transaction_id_generator_.NextId(), clock_,
      storage_.get(), lock_manager_.get(), versioned_catalog_.get(),
      action_manager_.get());
}

SchemaChangeContext Database::GetSchemaChangeContext() {
  return SchemaChangeContext{
      .type_factory = type_factory_.get(),
      .table_id_generator = &table_id_generator_,
      .column_id_generator = &column_id_generator_,
      .storage = storage_.get(),
      .pg_oid_assigner = pg_oid_assigner_.get(),
      .database_id = database_id_,
      .sequence_id_prefix = sequence_id_prefix_,
  };
}

absl::Status Database::UpdateSchema(
    const SchemaChangeOperation& schema_change_operation,
    int* num_succesful_statements, absl::Time* commit_timestamp,
    absl::Status* backfill_status) {
  if (schema_change_operation.statements.empty()) {
    return error::UpdateDatabaseMissingStatements();
  }
  // Whether it succeeds or not, a schema change frees the schemas it built on
  // the way, and the schema versions it expired.
  absl::Cleanup release_heap = [] { RequestHeapRelease(); };

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
  // Reserve a commit timestamp for the schema changes. Even if the
  // schema change fails, it will result in a no-op commit that will
  // be invisible to other read-only/read-write transactions.
  GOOGLESQL_ASSIGN_OR_RETURN(auto update_timestamp, lock.ReserveCommitTimestamp());

  auto context = GetSchemaChangeContext();
  context.schema_change_timestamp = update_timestamp;
  const Schema* existing_schema = versioned_catalog_->GetLatestSchema();
  SchemaUpdater updater;
  GOOGLESQL_ASSIGN_OR_RETURN(auto result,
                   updater.UpdateSchemaFromDDL(
                       existing_schema, schema_change_operation, context));
  *commit_timestamp = update_timestamp;
  *num_succesful_statements = result.num_successful_statements;
  *backfill_status = result.backfill_status;

  // We update the schema even if the backfill status was not OK, the returned
  // schema will be the schema for the last valid statement before the statement
  // for which the backfill/verification failed.
  if (result.updated_schema != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(versioned_catalog_->AddSchema(
        update_timestamp, std::move(result.updated_schema)));
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

int Database::RemoveExpiredSchemas() {
  return versioned_catalog_->RemoveExpiredSchemas(clock_->Now());
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
