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

#include "frontend/persistence/codec.h"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "backend/database/database.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/schema/printer/print_ddl.h"
#include "backend/storage/recording_storage.h"
#include "frontend/converters/types.h"
#include "frontend/converters/values.h"
#include "frontend/persistence/persistence.pb.h"
#include "google/spanner/v1/type.pb.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

absl::Status EncodeValue(const googlesql::Value& value, StoredValue* out) {
  out->Clear();
  if (!value.is_valid()) return absl::OkStatus();
  GOOGLESQL_RETURN_IF_ERROR(TypeToProto(value.type(), out->mutable_type()));
  GOOGLESQL_ASSIGN_OR_RETURN(*out->mutable_value(), ValueToProto(value));
  return absl::OkStatus();
}

absl::StatusOr<googlesql::Value> DecodeValue(
    const StoredValue& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle) {
  if (!in.has_type()) return googlesql::Value();
  const googlesql::Type* type = nullptr;
  GOOGLESQL_RETURN_IF_ERROR(
      TypeFromProto(in.type(), type_factory, &type, std::move(proto_bundle)));
  return ValueFromProto(in.value(), type);
}

absl::Status EncodeKey(const backend::Key& key, StoredKey* out) {
  out->Clear();
  for (int i = 0; i < key.NumColumns(); ++i) {
    GOOGLESQL_RETURN_IF_ERROR(
        EncodeValue(key.ColumnValue(i), out->add_columns()));
    out->add_descending(key.IsColumnDescending(i));
    out->add_nulls_last(key.IsColumnNullsLast(i));
  }
  out->set_infinity(key.is_infinity());
  out->set_prefix_limit(key.is_prefix_limit());
  return absl::OkStatus();
}

absl::StatusOr<backend::Key> DecodeKey(
    const StoredKey& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle) {
  if (in.infinity()) return backend::Key::Infinity();
  backend::Key key;
  for (int i = 0; i < in.columns_size(); ++i) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        googlesql::Value value,
        DecodeValue(in.columns(i), type_factory, proto_bundle));
    key.AddColumn(std::move(value),
                  i < in.descending_size() && in.descending(i),
                  i < in.nulls_last_size() && in.nulls_last(i));
  }
  if (in.prefix_limit()) return key.ToPrefixLimit();
  return key;
}

absl::Status EncodeOp(const backend::StorageOp& op, StorageOp* out) {
  out->Clear();
  if (const auto* write = std::get_if<backend::StorageWrite>(&op)) {
    out->set_table_id(write->table_id);
    StorageOp::Write* w = out->mutable_write();
    GOOGLESQL_RETURN_IF_ERROR(EncodeKey(write->key, w->mutable_key()));
    for (const auto& column_id : write->column_ids)
      w->add_column_ids(column_id);
    for (const auto& value : write->values) {
      GOOGLESQL_RETURN_IF_ERROR(EncodeValue(value, w->add_values()));
    }
    return absl::OkStatus();
  }
  const auto& del = std::get<backend::StorageDelete>(op);
  if (!del.key_range.IsClosedOpen()) {
    return absl::InternalError("persisted deletes must be ClosedOpen ranges");
  }
  out->set_table_id(del.table_id);
  StorageOp::Delete* d = out->mutable_delete_();
  GOOGLESQL_RETURN_IF_ERROR(
      EncodeKey(del.key_range.start_key(), d->mutable_start()));
  GOOGLESQL_RETURN_IF_ERROR(
      EncodeKey(del.key_range.limit_key(), d->mutable_limit()));
  return absl::OkStatus();
}

absl::StatusOr<backend::StorageOp> DecodeOp(
    const StorageOp& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle) {
  if (in.has_write()) {
    backend::StorageWrite write;
    write.table_id = in.table_id();
    GOOGLESQL_ASSIGN_OR_RETURN(
        write.key, DecodeKey(in.write().key(), type_factory, proto_bundle));
    for (const auto& column_id : in.write().column_ids()) {
      write.column_ids.push_back(column_id);
    }
    for (const auto& value : in.write().values()) {
      GOOGLESQL_ASSIGN_OR_RETURN(
          googlesql::Value v, DecodeValue(value, type_factory, proto_bundle));
      write.values.push_back(std::move(v));
    }
    if (write.values.size() != write.column_ids.size()) {
      return absl::DataLossError("persisted write has mismatched columns");
    }
    return write;
  }
  if (!in.has_delete_()) return absl::DataLossError("empty persisted op");
  GOOGLESQL_ASSIGN_OR_RETURN(
      backend::Key start,
      DecodeKey(in.delete_().start(), type_factory, proto_bundle));
  GOOGLESQL_ASSIGN_OR_RETURN(
      backend::Key limit,
      DecodeKey(in.delete_().limit(), type_factory, proto_bundle));
  return backend::StorageDelete{in.table_id(),
                                backend::KeyRange::ClosedOpen(start, limit)};
}

absl::Status EncodeSchema(const backend::PersistedSchema& schema,
                          SchemaState* out) {
  out->Clear();
  GOOGLESQL_ASSIGN_OR_RETURN(
      std::vector<std::string> statements,
      backend::PrintDDLStatements(schema.schema.get(),
                                  /*foreign_keys_last=*/true));
  for (auto& statement : statements) out->add_ddl(std::move(statement));
  std::shared_ptr<const backend::ProtoBundle> bundle =
      schema.schema->proto_bundle();
  if (bundle != nullptr && !bundle->types().empty()) {
    GOOGLESQL_ASSIGN_OR_RETURN(*out->mutable_proto_descriptors(),
                               bundle->GetProtoDescriptorBytes());
  }
  backend::SchemaIds ids = backend::CollectSchemaIds(schema.schema.get());
  out->mutable_table_ids()->insert(ids.tables.begin(), ids.tables.end());
  out->mutable_column_ids()->insert(ids.columns.begin(), ids.columns.end());
  out->mutable_sequence_ids()->insert(ids.sequences.begin(),
                                      ids.sequences.end());
  out->set_next_table_seq(schema.next_table_seq);
  out->set_next_column_seq(schema.next_column_seq);
  return absl::OkStatus();
}

backend::DatabaseRestore DecodeRestore(const SchemaState& schema) {
  backend::DatabaseRestore restore;
  restore.ids.tables.insert(schema.table_ids().begin(),
                            schema.table_ids().end());
  restore.ids.columns.insert(schema.column_ids().begin(),
                             schema.column_ids().end());
  restore.ids.sequences.insert(schema.sequence_ids().begin(),
                               schema.sequence_ids().end());
  restore.next_table_seq = schema.next_table_seq();
  restore.next_column_seq = schema.next_column_seq();
  return restore;
}

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
