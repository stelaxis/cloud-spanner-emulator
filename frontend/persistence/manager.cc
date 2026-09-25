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

#include "frontend/persistence/manager.h"

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <variant>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/database/database_log.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/updater/schema_updater.h"
#include "backend/storage/iterator.h"
#include "backend/storage/recording_storage.h"
#include "backend/storage/storage.h"
#include "common/clock.h"
#include "frontend/common/uris.h"
#include "frontend/entities/database.h"
#include "frontend/persistence/codec.h"
#include "frontend/persistence/log.h"
#include "frontend/persistence/persistence.pb.h"
#include "google/spanner/admin/database/v1/common.pb.h"
#include "google/spanner/admin/instance/v1/spanner_instance_admin.pb.h"
#include "googlesql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

namespace {

namespace database_api = ::google::spanner::admin::database::v1;
namespace instance_api = ::google::spanner::admin::instance::v1;

std::atomic<bool> checkpoint_signalled{false};

void OnCheckpointSignal(int) { checkpoint_signalled.store(true); }

class DatabaseLogImpl : public backend::DatabaseLog {
 public:
  DatabaseLogImpl(PersistenceManager* manager, uint64_t incarnation)
      : manager_(manager), incarnation_(incarnation) {}

  absl::Mutex* commit_gate() override { return manager_->gate(); }

  absl::Status health() override { return manager_->health(); }

  absl::Status LogCommit(absl::Time commit_timestamp,
                         const std::vector<backend::StorageOp>& ops) override {
    Record record;
    record.set_timestamp_nanos(ToNanos(commit_timestamp));
    Commit* commit = record.mutable_commit();
    commit->set_database(incarnation_);
    for (const auto& op : ops) {
      GOOGLESQL_RETURN_IF_ERROR(EncodeOp(op, commit->add_ops()));
    }
    return manager_->Append(record);
  }

  absl::Status LogSchemaChange(
      absl::Time commit_timestamp, const backend::PersistedSchema& schema,
      const std::vector<backend::StorageOp>& ops) override {
    Record record;
    record.set_timestamp_nanos(ToNanos(commit_timestamp));
    SchemaChange* change = record.mutable_schema_change();
    change->set_database(incarnation_);
    GOOGLESQL_RETURN_IF_ERROR(EncodeSchema(schema, change->mutable_schema()));
    for (const auto& op : ops) {
      GOOGLESQL_RETURN_IF_ERROR(EncodeOp(op, change->add_ops()));
    }
    return manager_->Append(record);
  }

  uint64_t incarnation() const { return incarnation_; }

 private:
  PersistenceManager* const manager_;
  const uint64_t incarnation_;
};

uint64_t IncarnationOf(const Database& database) {
  return static_cast<const DatabaseLogImpl*>(database.backend()->log())
      ->incarnation();
}

// The tables with stored rows: user tables, index data tables and change
// stream tables.
std::vector<const backend::Table*> StoredTables(const backend::Schema* schema) {
  std::vector<const backend::Table*> tables;
  for (const backend::Table* table : schema->tables()) {
    tables.push_back(table);
    for (const backend::Index* index : table->indexes()) {
      if (index->index_data_table() != nullptr) {
        tables.push_back(index->index_data_table());
      }
    }
  }
  for (const backend::ChangeStream* change_stream : schema->change_streams()) {
    for (const backend::Table* table :
         {change_stream->change_stream_data_table(),
          change_stream->change_stream_partition_table()}) {
      if (table != nullptr) tables.push_back(table);
    }
  }
  return tables;
}

// A recovered database: its latest schema, checkpointed rows and the
// effects logged after the checkpoint.
struct DatabaseImage {
  DatabaseState state;
  const DatabaseCheckpoint* checkpoint = nullptr;
  std::vector<std::pair<absl::Time,
                        const google::protobuf::RepeatedPtrField<StorageOp>*>>
      ops;
};

// Loads recovered rows into `database`, skipping tables and columns that are
// no longer in its schema.
absl::Status LoadRows(const DatabaseImage& image, absl::Time data_timestamp,
                      backend::Database* database) {
  std::shared_ptr<const backend::Schema> schema =
      database->GetLatestSchemaShared();
  absl::flat_hash_map<std::string, absl::flat_hash_set<std::string>> live;
  for (const backend::Table* table : StoredTables(schema.get())) {
    auto& columns = live[table->id()];
    for (const backend::Column* column : table->columns()) {
      columns.insert(column->id());
    }
  }
  googlesql::TypeFactory* type_factory =
      database->query_engine()->type_factory();
  std::shared_ptr<const backend::ProtoBundle> bundle = schema->proto_bundle();
  backend::Storage* storage = database->storage();

  if (image.checkpoint != nullptr) {
    for (const TableRows& table : image.checkpoint->tables()) {
      auto columns = live.find(table.table_id());
      if (columns == live.end()) continue;
      for (const TableRows::Row& row : table.rows()) {
        GOOGLESQL_ASSIGN_OR_RETURN(backend::Key key,
                                   DecodeKey(row.key(), type_factory, bundle));
        std::vector<std::string> column_ids;
        std::vector<googlesql::Value> values;
        for (int i = 0; i < row.values_size() && i < table.column_ids_size();
             ++i) {
          if (!row.values(i).has_type() ||
              !columns->second.contains(table.column_ids(i))) {
            continue;
          }
          GOOGLESQL_ASSIGN_OR_RETURN(
              googlesql::Value value,
              DecodeValue(row.values(i), type_factory, bundle));
          column_ids.push_back(table.column_ids(i));
          values.push_back(std::move(value));
        }
        GOOGLESQL_RETURN_IF_ERROR(storage->Write(
            data_timestamp, table.table_id(), key, column_ids, values));
      }
    }
  }
  for (const auto& [timestamp, ops] : image.ops) {
    for (const StorageOp& stored : *ops) {
      auto columns = live.find(stored.table_id());
      if (columns == live.end()) continue;
      // Drop the values of columns that are gone before decoding: their
      // types (a proto or enum) may be gone from the schema too.
      StorageOp kept;
      const StorageOp* op = &stored;
      if (stored.has_write()) {
        kept.set_table_id(stored.table_id());
        StorageOp::Write* write = kept.mutable_write();
        *write->mutable_key() = stored.write().key();
        for (int i = 0; i < stored.write().column_ids_size() &&
                        i < stored.write().values_size();
             ++i) {
          if (columns->second.contains(stored.write().column_ids(i))) {
            write->add_column_ids(stored.write().column_ids(i));
            *write->add_values() = stored.write().values(i);
          }
        }
        op = &kept;
      }
      GOOGLESQL_ASSIGN_OR_RETURN(backend::StorageOp decoded,
                                 DecodeOp(*op, type_factory, bundle));
      GOOGLESQL_RETURN_IF_ERROR(
          backend::ApplyStorageOps(timestamp, {decoded}, storage));
    }
  }
  return absl::OkStatus();
}

}  // namespace

void InstallCheckpointSignalHandler() {
  struct sigaction action = {};
  action.sa_handler = OnCheckpointSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  sigaction(SIGUSR1, &action, nullptr);
}

absl::StatusOr<std::unique_ptr<PersistenceManager>> PersistenceManager::Open(
    const PersistenceOptions& options, Clock* clock,
    InstanceManager* instance_manager, DatabaseManager* database_manager) {
  auto manager = absl::WrapUnique(new PersistenceManager(options, clock));
  LogContents contents;
  GOOGLESQL_ASSIGN_OR_RETURN(manager->log_,
                             Log::Open(options.fs, options.dir, &contents));
  if (contents.discarded_tail_bytes > 0) {
    ABSL_LOG(WARNING) << "Discarded a torn final log record ("
                      << contents.discarded_tail_bytes << " bytes) in "
                      << options.dir;
  }
  GOOGLESQL_RETURN_IF_ERROR(
      manager->Recover(contents, instance_manager, database_manager));
  instance_manager->set_persistence(manager.get());
  database_manager->set_persistence(manager.get());
  if (options.background_checkpoints) {
    manager->thread_ =
        std::thread([m = manager.get()] { m->CheckpointLoop(); });
  }
  return manager;
}

absl::Status PersistenceManager::Recover(const LogContents& contents,
                                         InstanceManager* instance_manager,
                                         DatabaseManager* database_manager) {
  persistence::Checkpoint checkpoint;
  if (contents.checkpoint.has_value() &&
      !checkpoint.ParseFromString(*contents.checkpoint)) {
    return absl::DataLossError("the checkpoint cannot be parsed");
  }
  std::vector<Record> records(contents.records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    if (!records[i].ParseFromString(contents.records[i])) {
      return absl::DataLossError(absl::StrCat(
          "log record ", contents.boundary + i, " cannot be parsed"));
    }
  }

  // Replay the checkpoint image and then every record after its boundary.
  std::map<uint64_t, InstanceState> instances;
  std::map<uint64_t, DatabaseImage> databases;
  absl::flat_hash_map<std::string, int64_t> reservations;
  uint64_t next_incarnation =
      std::max<uint64_t>(1, checkpoint.next_incarnation());
  int64_t high = std::max(checkpoint.clock_high_nanos(),
                          checkpoint.data_timestamp_nanos());
  for (const InstanceState& instance : checkpoint.instances()) {
    instances[instance.incarnation()] = instance;
  }
  for (const DatabaseCheckpoint& database : checkpoint.databases()) {
    DatabaseImage& image = databases[database.database().incarnation()];
    image.state = database.database();
    image.checkpoint = &database;
  }
  for (const auto& [id, end] : checkpoint.sequence_reservations()) {
    reservations[id] = end;
  }
  int64_t last_timestamp = checkpoint.data_timestamp_nanos();
  for (size_t i = 0; i < records.size(); ++i) {
    const Record& record = records[i];
    int64_t ts = record.timestamp_nanos();
    if (ts != 0) {
      if (ts <= last_timestamp) {
        return absl::DataLossError(
            absl::StrCat("log record ", contents.boundary + i,
                         " is out of commit timestamp order"));
      }
      last_timestamp = ts;
      high = std::max(high, ts);
    }
    switch (record.effect_case()) {
      case Record::kCommit:
        if (auto it = databases.find(record.commit().database());
            it != databases.end()) {
          it->second.ops.emplace_back(FromNanos(ts), &record.commit().ops());
        }
        break;
      case Record::kSchemaChange:
        if (auto it = databases.find(record.schema_change().database());
            it != databases.end()) {
          *it->second.state.mutable_schema() = record.schema_change().schema();
          it->second.ops.emplace_back(FromNanos(ts),
                                      &record.schema_change().ops());
        }
        break;
      case Record::kCreateInstance:
        instances[record.create_instance().incarnation()] =
            record.create_instance();
        next_incarnation = std::max(next_incarnation,
                                    record.create_instance().incarnation() + 1);
        break;
      case Record::kDropInstance:
        instances.erase(record.drop_instance());
        for (auto it = databases.begin(); it != databases.end();) {
          it = it->second.state.instance_incarnation() == record.drop_instance()
                   ? databases.erase(it)
                   : std::next(it);
        }
        break;
      case Record::kCreateDatabase: {
        DatabaseImage& image =
            databases[record.create_database().incarnation()];
        image = DatabaseImage();
        image.state = record.create_database();
        next_incarnation = std::max(next_incarnation,
                                    record.create_database().incarnation() + 1);
        break;
      }
      case Record::kDropDatabase:
        databases.erase(record.drop_database());
        break;
      case Record::kSequenceReservation: {
        int64_t& end =
            reservations[record.sequence_reservation().sequence_id()];
        end = std::max(end, record.sequence_reservation().end());
        break;
      }
      case Record::kClockLeaseNanos:
        high = std::max(high, record.clock_lease_nanos());
        break;
      default:
        return absl::DataLossError(absl::StrCat(
            "log record ", contents.boundary + i, " has an unknown effect"));
    }
  }

  // Restart above every timestamp handed out before: commit timestamps,
  // leases (which bound served read timestamps) and the wall clock.
  absl::Time restart = std::max(FromNanos(high), clock_->Now());
  clock_->AdvanceTo(restart);
  restart_floor_ = restart + absl::Microseconds(1);
  // No timestamp after `restart` is handed out before a durable lease covers
  // it.
  clock_->SetLease(restart,
                   [this](absl::Time needed) -> absl::StatusOr<absl::Time> {
                     absl::Time lease = needed + options_.lease_window;
                     Record record;
                     record.set_clock_lease_nanos(ToNanos(lease));
                     GOOGLESQL_RETURN_IF_ERROR(Append(record));
                     return lease;
                   });

  for (const auto& [incarnation, instance] : instances) {
    instance_api::Instance proto;
    proto.set_config(instance.config());
    proto.set_display_name(instance.display_name());
    proto.set_processing_units(instance.processing_units());
    proto.mutable_labels()->insert(instance.labels().begin(),
                                   instance.labels().end());
    GOOGLESQL_RETURN_IF_ERROR(
        instance_manager->CreateInstance(instance.uri(), proto).status());
    absl::MutexLock lock(catalog_mu_);
    instances_[instance.uri()] = instance;
  }

  absl::flat_hash_set<std::string> live_sequences;
  for (const auto& [incarnation, image] : databases) {
    absl::string_view project_id, instance_id, database_id;
    GOOGLESQL_RETURN_IF_ERROR(ParseDatabaseUri(image.state.uri(), &project_id,
                                               &instance_id, &database_id));
    const SchemaState& schema = image.state.schema();
    backend::DatabaseRestore restore = DecodeRestore(schema);
    std::vector<std::string> statements(schema.ddl().begin(),
                                        schema.ddl().end());
    auto backend_database = backend::Database::Create(
        clock_, database_id,
        backend::SchemaChangeOperation{
            .statements = statements,
            .proto_descriptor_bytes = schema.proto_descriptors(),
            .database_dialect =
                database_api::DatabaseDialect::GOOGLE_STANDARD_SQL},
        std::make_unique<DatabaseLogImpl>(this, incarnation), &restore);
    if (!backend_database.ok()) {
      return absl::Status(
          backend_database.status().code(),
          absl::StrCat("Recovering database ", image.state.uri(), ": ",
                       backend_database.status().message()));
    }
    GOOGLESQL_RETURN_IF_ERROR(
        LoadRows(image, FromNanos(checkpoint.data_timestamp_nanos()),
                 backend_database->get()));
    (*backend_database)->SetRestartFloor(restart_floor_);
    for (const auto& [name, id] : restore.ids.sequences) {
      live_sequences.insert(id);
    }
    auto database = std::make_shared<Database>(
        image.state.uri(), *std::move(backend_database),
        FromNanos(image.state.create_time_nanos()));
    GOOGLESQL_RETURN_IF_ERROR(database_manager->RestoreDatabase(database));
    absl::MutexLock lock(catalog_mu_);
    LiveDatabase& live = databases_[incarnation];
    live.state = image.state;
    live.state.clear_schema();
    live.database = database;
  }
  for (const auto& [id, end] : reservations) {
    if (live_sequences.contains(id))
      backend::Sequence::RestoreReservation(id, end);
  }
  {
    absl::MutexLock lock(catalog_mu_);
    next_incarnation_ = next_incarnation;
  }

  // No sequence value is handed out before a durable reservation covers it.
  backend::Sequence::SetReservationHook(
      [this](const std::string& sequence_id, int64_t end) {
        Record record;
        record.mutable_sequence_reservation()->set_sequence_id(sequence_id);
        record.mutable_sequence_reservation()->set_end(end);
        return Append(record);
      });
  if (!contents.records.empty()) {
    absl::MutexLock lock(thread_mu_);
    checkpoint_wanted_ = contents.records.size() > 1000;
  }
  return absl::OkStatus();
}

void PersistenceManager::StopCheckpoints() {
  {
    absl::MutexLock lock(thread_mu_);
    stop_ = true;
  }
  if (thread_.joinable()) thread_.join();
}

PersistenceManager::~PersistenceManager() {
  StopCheckpoints();
  backend::Sequence::SetReservationHook(nullptr);
  clock_->SetLease(absl::InfiniteFuture(), nullptr);
}

absl::Status PersistenceManager::Append(const Record& record) {
  GOOGLESQL_RETURN_IF_ERROR(log_->Append(record.SerializeAsString()).status());
  if (log_->CurrentSegmentBytes() >=
      static_cast<uint64_t>(options_.checkpoint_bytes)) {
    absl::MutexLock lock(thread_mu_);
    checkpoint_wanted_ = true;
  }
  return absl::OkStatus();
}

void PersistenceManager::CheckpointLoop() {
  while (true) {
    bool wanted;
    {
      absl::MutexLock lock(thread_mu_);
      auto ready = [this]() { return stop_ || checkpoint_wanted_; };
      thread_mu_.AwaitWithTimeout(absl::Condition(&ready),
                                  absl::Milliseconds(100));
      if (stop_) return;
      wanted = checkpoint_wanted_ || checkpoint_signalled.exchange(false);
      checkpoint_wanted_ = false;
    }
    if (!wanted) continue;
    if (auto status = Checkpoint(); !status.ok()) {
      ABSL_LOG(WARNING) << "Checkpoint failed: " << status;
    } else {
      ABSL_LOG(INFO) << "Checkpoint written to " << options_.dir;
    }
  }
}

absl::Status PersistenceManager::Checkpoint() {
  absl::MutexLock checkpoint_lock(checkpoint_mu_);
  persistence::Checkpoint checkpoint;
  uint64_t boundary;
  {
    // With the gate held no commit is in flight, so the live state is exactly
    // what the log holds below the boundary.
    absl::MutexLock gate_lock(gate_);
    GOOGLESQL_RETURN_IF_ERROR(log_->health());
    GOOGLESQL_ASSIGN_OR_RETURN(boundary, log_->StartSegment());
    absl::Time data_timestamp = clock_->Now();
    checkpoint.set_data_timestamp_nanos(ToNanos(data_timestamp));
    // Read after StartSegment: a lease or sequence reservation logged before
    // the boundary is reflected here, and any later one is after it.
    checkpoint.set_clock_high_nanos(
        ToNanos(std::max(data_timestamp, clock_->lease())));
    absl::flat_hash_map<std::string, int64_t> reservations =
        backend::Sequence::ReservationEnds();

    absl::MutexLock catalog_lock(catalog_mu_);
    checkpoint.set_next_incarnation(next_incarnation_);
    for (const auto& [uri, instance] : instances_) {
      *checkpoint.add_instances() = instance;
    }
    for (const auto& [incarnation, live] : databases_) {
      std::shared_ptr<Database> database = live.database.lock();
      if (database == nullptr) {
        // Only a teardown in the wrong order gets here. Skipping the
        // database would checkpoint it, and its log, out of existence.
        return absl::FailedPreconditionError(absl::StrCat(
            "Database ", live.state.uri(), " is gone but was not dropped"));
      }
      DatabaseCheckpoint* out = checkpoint.add_databases();
      *out->mutable_database() = live.state;
      backend::PersistedSchema schema =
          database->backend()->GetPersistedSchema();
      GOOGLESQL_RETURN_IF_ERROR(
          EncodeSchema(schema, out->mutable_database()->mutable_schema()));
      for (const auto& [name, id] : out->database().schema().sequence_ids()) {
        if (auto it = reservations.find(id); it != reservations.end()) {
          (*checkpoint.mutable_sequence_reservations())[id] = it->second;
        }
      }
      backend::Storage* storage = database->backend()->storage();
      for (const backend::Table* table : StoredTables(schema.schema.get())) {
        TableRows* rows = out->add_tables();
        rows->set_table_id(table->id());
        std::vector<std::string> column_ids;
        for (const backend::Column* column : table->columns()) {
          column_ids.push_back(column->id());
          rows->add_column_ids(column->id());
        }
        std::unique_ptr<backend::StorageIterator> itr;
        GOOGLESQL_RETURN_IF_ERROR(
            storage->Read(data_timestamp, table->id(),
                          backend::KeyRange::ClosedOpen(
                              backend::Key::Empty(), backend::Key::Infinity()),
                          column_ids, &itr));
        while (itr->Next()) {
          TableRows::Row* row = rows->add_rows();
          GOOGLESQL_RETURN_IF_ERROR(EncodeKey(itr->Key(), row->mutable_key()));
          for (int i = 0; i < itr->NumColumns(); ++i) {
            GOOGLESQL_RETURN_IF_ERROR(
                EncodeValue(itr->ColumnValue(i), row->add_values()));
          }
        }
        GOOGLESQL_RETURN_IF_ERROR(itr->Status());
      }
    }
  }
  GOOGLESQL_RETURN_IF_ERROR(
      log_->PublishCheckpoint(boundary, checkpoint.SerializeAsString()));
  return log_->RemoveSegmentsBelow(boundary);
}

std::unique_ptr<backend::DatabaseLog> PersistenceManager::NewDatabaseLog() {
  absl::MutexLock lock(catalog_mu_);
  return std::make_unique<DatabaseLogImpl>(this, next_incarnation_++);
}

absl::Status PersistenceManager::LogCreateInstance(
    const std::string& instance_uri, const instance_api::Instance& instance) {
  absl::MutexLock gate_lock(gate_);
  InstanceState state;
  {
    absl::MutexLock lock(catalog_mu_);
    state.set_incarnation(next_incarnation_++);
  }
  state.set_uri(instance_uri);
  state.set_config(instance.config());
  state.set_display_name(instance.display_name());
  state.set_processing_units(instance.processing_units());
  state.mutable_labels()->insert(instance.labels().begin(),
                                 instance.labels().end());
  Record record;
  record.set_timestamp_nanos(ToNanos(clock_->Now()));
  *record.mutable_create_instance() = state;
  GOOGLESQL_RETURN_IF_ERROR(Append(record));
  absl::MutexLock lock(catalog_mu_);
  instances_[instance_uri] = state;
  return absl::OkStatus();
}

absl::Status PersistenceManager::LogDeleteInstance(
    const std::string& instance_uri) {
  absl::MutexLock gate_lock(gate_);
  uint64_t incarnation;
  {
    absl::MutexLock lock(catalog_mu_);
    auto it = instances_.find(instance_uri);
    if (it == instances_.end()) return absl::OkStatus();
    incarnation = it->second.incarnation();
  }
  Record record;
  record.set_timestamp_nanos(ToNanos(clock_->Now()));
  record.set_drop_instance(incarnation);
  GOOGLESQL_RETURN_IF_ERROR(Append(record));
  absl::MutexLock lock(catalog_mu_);
  instances_.erase(instance_uri);
  for (auto it = databases_.begin(); it != databases_.end();) {
    it = it->second.state.instance_incarnation() == incarnation
             ? databases_.erase(it)
             : std::next(it);
  }
  return absl::OkStatus();
}

absl::Status PersistenceManager::LogCreateDatabase(
    const std::string& instance_uri, const std::shared_ptr<Database>& database,
    absl::Time create_time) {
  absl::MutexLock gate_lock(gate_);
  DatabaseState state;
  {
    absl::MutexLock lock(catalog_mu_);
    auto it = instances_.find(instance_uri);
    if (it == instances_.end()) {
      return absl::NotFoundError(
          absl::StrCat("Instance not found: ", instance_uri));
    }
    state.set_instance_incarnation(it->second.incarnation());
  }
  state.set_incarnation(IncarnationOf(*database));
  state.set_uri(database->database_uri());
  state.set_create_time_nanos(ToNanos(create_time));
  Record record;
  record.set_timestamp_nanos(ToNanos(clock_->Now()));
  *record.mutable_create_database() = state;
  GOOGLESQL_RETURN_IF_ERROR(
      EncodeSchema(database->backend()->GetPersistedSchema(),
                   record.mutable_create_database()->mutable_schema()));
  GOOGLESQL_RETURN_IF_ERROR(Append(record));
  absl::MutexLock lock(catalog_mu_);
  databases_[state.incarnation()] = LiveDatabase{state, database};
  return absl::OkStatus();
}

absl::Status PersistenceManager::LogDropDatabase(const Database& database) {
  absl::MutexLock gate_lock(gate_);
  uint64_t incarnation = IncarnationOf(database);
  Record record;
  record.set_timestamp_nanos(ToNanos(clock_->Now()));
  record.set_drop_database(incarnation);
  GOOGLESQL_RETURN_IF_ERROR(Append(record));
  absl::MutexLock lock(catalog_mu_);
  databases_.erase(incarnation);
  return absl::OkStatus();
}

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
