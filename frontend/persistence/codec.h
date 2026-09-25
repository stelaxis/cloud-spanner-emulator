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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_CODEC_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_CODEC_H_

#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "backend/database/database.h"
#include "backend/database/database_log.h"
#include "backend/datamodel/key.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/storage/recording_storage.h"
#include "frontend/persistence/persistence.pb.h"
#include "google/protobuf/timestamp.pb.h"
#include "googlesql/public/types/type_factory.h"
#include "googlesql/public/value.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

// Values use the client wire encoding with their own type, so a value
// decodes without its column's schema.
absl::Status EncodeValue(const googlesql::Value& value, StoredValue* out);
absl::StatusOr<googlesql::Value> DecodeValue(
    const StoredValue& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle);

absl::Status EncodeKey(const backend::Key& key, StoredKey* out);
absl::StatusOr<backend::Key> DecodeKey(
    const StoredKey& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle);

absl::Status EncodeOp(const backend::StorageOp& op, StorageOp* out);
absl::StatusOr<backend::StorageOp> DecodeOp(
    const StorageOp& in, googlesql::TypeFactory* type_factory,
    std::shared_ptr<const backend::ProtoBundle> proto_bundle);

// The schema's DDL, proto descriptors and storage IDs.
absl::Status EncodeSchema(const backend::PersistedSchema& schema,
                          SchemaState* out);

// What rebuilding the schema needs to give its objects their IDs back.
backend::DatabaseRestore DecodeRestore(const SchemaState& schema);

// Lossless for every timestamp Spanner accepts.
absl::Status EncodeTime(absl::Time time, google::protobuf::Timestamp* out);
absl::Time DecodeTime(const google::protobuf::Timestamp& in);

// The timestamps of a record or checkpoint, from the current field or, in
// format version 1 files, the int64 nanosecond one. A record without a
// commit timestamp gives InfinitePast.
absl::Time RecordTimestamp(const Record& record);
absl::Time LeaseOf(const Record& record);
absl::Time CreateTimeOf(const DatabaseState& database);
absl::Time ClockHighOf(const Checkpoint& checkpoint);
absl::Time DataTimestampOf(const Checkpoint& checkpoint);

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_CODEC_H_
