//
// Copyright 2026 Google LLC
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

#include "backend/database/schema_create_cache.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "googlesql/public/function_signature.h"
#include "googlesql/public/functions/uuid.h"
#include "googlesql/public/type.h"
#include "googlesql/public/types/type_factory.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/commandlineflag.h"
#include "absl/flags/flag.h"
#include "absl/flags/reflection.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "backend/schema/builders/change_stream_builder.h"
#include "backend/schema/builders/column_builder.h"
#include "backend/schema/builders/model_builder.h"
#include "backend/schema/builders/sequence_builder.h"
#include "backend/schema/builders/view_builder.h"
#include "backend/schema/catalog/change_stream.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/model.h"
#include "backend/schema/catalog/proto_bundle.h"
#include "backend/schema/catalog/schema.h"
#include "backend/schema/catalog/sequence.h"
#include "backend/schema/catalog/udf.h"
#include "backend/schema/catalog/view.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "backend/schema/graph/schema_node.h"
#include "backend/schema/updater/schema_validation_context.h"
#include "common/feature_flags.h"
#include "googlesql/base/ret_check.h"
#include "googlesql/base/status_macros.h"

ABSL_FLAG(int, schema_create_cache_size, 8,
          "The number of schemas that CreateDatabase remembers, by the DDL "
          "statements and settings they were created with. A CreateDatabase "
          "with the same statements and settings as a remembered successful "
          "one copies its schema instead of processing the statements again. "
          "0 disables the cache.");

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Simple types and the PostgreSQL types are static and owned by no type
// factory; every other type in a schema is made by the database's factory.
bool IsStaticType(const googlesql::Type* type) {
  return type == nullptr || type->IsSimpleType() || type->IsExtendedType();
}

absl::StatusOr<const googlesql::Type*> CopyType(
    const googlesql::Type* type, googlesql::TypeFactory* type_factory,
    const ProtoBundle& proto_bundle) {
  if (IsStaticType(type)) {
    return type;
  }
  const googlesql::Type* copy = nullptr;
  if (type->IsArray()) {
    GOOGLESQL_ASSIGN_OR_RETURN(
        const googlesql::Type* element,
        CopyType(type->AsArray()->element_type(), type_factory, proto_bundle));
    GOOGLESQL_RETURN_IF_ERROR(type_factory->MakeArrayType(element, &copy));
    return copy;
  }
  if (type->IsStruct()) {
    std::vector<googlesql::StructType::StructField> fields;
    for (const googlesql::StructType::StructField& field :
         type->AsStruct()->fields()) {
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* field_type,
                       CopyType(field.type, type_factory, proto_bundle));
      fields.push_back({field.name, field_type});
    }
    GOOGLESQL_RETURN_IF_ERROR(type_factory->MakeStructType(fields, &copy));
    return copy;
  }
  if (type->IsProto()) {
    GOOGLESQL_ASSIGN_OR_RETURN(const google::protobuf::Descriptor* descriptor,
                     proto_bundle.GetTypeDescriptor(
                         type->AsProto()->descriptor()->full_name()));
    GOOGLESQL_RETURN_IF_ERROR(type_factory->MakeProtoType(descriptor, &copy));
    return copy;
  }
  if (type->IsEnum()) {
    GOOGLESQL_ASSIGN_OR_RETURN(const google::protobuf::EnumDescriptor* descriptor,
                     proto_bundle.GetEnumTypeDescriptor(
                         type->AsEnum()->enum_descriptor()->full_name()));
    GOOGLESQL_RETURN_IF_ERROR(type_factory->MakeEnumType(descriptor, &copy));
    return copy;
  }
  return absl::UnimplementedError(
      absl::StrCat("Cannot copy type ", type->DebugString()));
}

absl::StatusOr<std::shared_ptr<const ProtoBundle>> CopyProtoBundle(
    const ProtoBundle& proto_bundle) {
  if (proto_bundle.empty()) {
    return ProtoBundle::CreateEmpty();
  }
  GOOGLESQL_ASSIGN_OR_RETURN(std::string descriptors,
                   proto_bundle.GetProtoDescriptorBytes());
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<ProtoBundle::Builder> builder,
                   ProtoBundle::Builder::New(descriptors));
  GOOGLESQL_RETURN_IF_ERROR(builder->InsertTypes(std::vector<std::string>(
      proto_bundle.types().begin(), proto_bundle.types().end())));
  return builder->Build();
}

// A function's signature is copied as is, so it may only have static types
// and no default arguments, whose values could have other types.
absl::Status CheckUdfCopyable(const Udf& udf) {
  const googlesql::FunctionSignature* signature = udf.signature();
  if (signature == nullptr) {
    return absl::OkStatus();
  }
  std::vector<const googlesql::FunctionArgumentType*> types = {
      &signature->result_type()};
  for (const googlesql::FunctionArgumentType& argument :
       signature->arguments()) {
    types.push_back(&argument);
  }
  for (const googlesql::FunctionArgumentType* argument : types) {
    if (!IsStaticType(argument->type()) || argument->HasDefault()) {
      return absl::UnimplementedError(
          absl::StrCat("Cannot copy the signature of function ", udf.Name()));
    }
  }
  return absl::OkStatus();
}

// Mutable access to `node` as a T: `node` is a clone that CopySchema owns.
template <typename T>
T* Mutable(const SchemaNode* node) {
  return const_cast<T*>(node->As<const T>());
}

absl::Status CopyNode(const SchemaNode* node,
                      googlesql::TypeFactory* type_factory,
                      const ProtoBundle& proto_bundle,
                      std::string_view sequence_id_prefix,
                      std::optional<absl::Time> now) {
  if (auto* column = Mutable<Column>(node); column != nullptr) {
    GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* type,
                     CopyType(column->GetType(), type_factory, proto_bundle));
    Column::Editor(column).set_type(type);
  } else if (auto* view = Mutable<View>(node); view != nullptr) {
    std::vector<View::Column> columns;
    for (const View::Column& view_column : view->columns()) {
      GOOGLESQL_ASSIGN_OR_RETURN(const googlesql::Type* type,
                       CopyType(view_column.type, type_factory, proto_bundle));
      columns.emplace_back(view_column.name, type);
    }
    View::Editor(view).set_columns(std::move(columns));
  } else if (auto* model = Mutable<Model>(node); model != nullptr) {
    auto copy_columns = [&](absl::Span<const Model::ModelColumn> columns)
        -> absl::StatusOr<std::vector<Model::ModelColumn>> {
      std::vector<Model::ModelColumn> copies(columns.begin(), columns.end());
      for (Model::ModelColumn& copy : copies) {
        GOOGLESQL_ASSIGN_OR_RETURN(copy.type,
                         CopyType(copy.type, type_factory, proto_bundle));
      }
      return copies;
    };
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Model::ModelColumn> input,
                     copy_columns(model->input()));
    GOOGLESQL_ASSIGN_OR_RETURN(std::vector<Model::ModelColumn> output,
                     copy_columns(model->output()));
    Model::Editor(model).set_input(std::move(input)).set_output(
        std::move(output));
  } else if (const Udf* udf = node->As<const Udf>(); udf != nullptr) {
    GOOGLESQL_RETURN_IF_ERROR(CheckUdfCopyable(*udf));
  } else if (auto* sequence = Mutable<Sequence>(node); sequence != nullptr) {
    // As CREATE SEQUENCE assigns them. A sequence's state is kept by ID across
    // databases, and a new ID has none.
    absl::BitGen bitgen;
    Sequence::Editor(sequence).set_id(absl::StrCat(
        sequence_id_prefix, googlesql::functions::GenerateUuid(bitgen)));
  } else if (auto* change_stream = Mutable<ChangeStream>(node);
             change_stream != nullptr && now.has_value()) {
    ChangeStream::Editor(change_stream).set_creation_time(*now);
  }
  return absl::OkStatus();
}

std::atomic<SchemaCreateCache*> global_for_testing{nullptr};
std::atomic<bool> global_overridden{false};

static_assert(std::has_unique_object_representations_v<
                  EmulatorFeatureFlags::Flags>,
              "Settings compare the flags' bytes");

}  // namespace

absl::StatusOr<std::unique_ptr<const Schema>> CopySchema(
    const Schema& schema, googlesql::TypeFactory* type_factory,
    std::string_view database_id, std::string_view sequence_id_prefix,
    std::optional<absl::Time> now) {
  GOOGLESQL_ASSIGN_OR_RETURN(std::shared_ptr<const ProtoBundle> proto_bundle,
                   CopyProtoBundle(*schema.proto_bundle()));
  SchemaValidationContext context;
  SchemaGraphEditor editor(schema.GetSchemaGraph(), &context);
  GOOGLESQL_ASSIGN_OR_RETURN(std::unique_ptr<SchemaGraph> graph, editor.CloneGraph());
  for (const SchemaNode* node : graph->GetSchemaNodes()) {
    GOOGLESQL_RETURN_IF_ERROR(
        CopyNode(node, type_factory, *proto_bundle, sequence_id_prefix, now));
  }
  return std::make_unique<const OwningSchema>(
      std::move(graph), std::move(proto_bundle), schema.dialect(),
      database_id);
}

SchemaCreateCache* SchemaCreateCache::Global() {
  if (global_overridden.load()) {
    return global_for_testing.load();
  }
  static SchemaCreateCache* global = [] {
    int size = absl::GetFlag(FLAGS_schema_create_cache_size);
    return size > 0 ? new SchemaCreateCache(size) : nullptr;
  }();
  return global;
}

SchemaCreateCache* SchemaCreateCache::SetGlobalForTesting(
    SchemaCreateCache* cache) {
  SchemaCreateCache* previous = Global();
  global_for_testing = cache;
  global_overridden = true;
  return previous;
}

namespace {

// Flags that DDL processing never reads and that change at runtime: every
// database's change stream churner sets its intervals when
// --override_change_stream_partition_token_alive_seconds is set, and tests set
// the ephemeral session limits. This cache's size is read once.
bool IsExcludedFromSettings(absl::string_view flag_name) {
  static const auto* excluded = new absl::flat_hash_set<absl::string_view>{
      "change_stream_churning_interval",
      "change_stream_churn_thread_sleep_interval",
      "change_stream_churn_thread_retry_sleep_interval",
      "schema_create_cache_size",
      "ephemeral_session_idle_timeout_seconds",
      "max_ephemeral_sessions",
  };
  return excluded->contains(flag_name);
}

// The flags in the settings, by name. The flags linked into a process are
// registered before main() runs and never change.
const std::vector<absl::CommandLineFlag*>& SettingsFlags() {
  static const auto* flags = [] {
    auto* flags = new std::vector<absl::CommandLineFlag*>();
    for (const auto& [name, flag] : absl::GetAllFlags()) {
      if (!IsExcludedFromSettings(name)) flags->push_back(flag);
    }
    std::sort(flags->begin(), flags->end(),
              [](const absl::CommandLineFlag* a,
                 const absl::CommandLineFlag* b) {
                return a->Name() < b->Name();
              });
    return flags;
  }();
  return *flags;
}

std::string SettingsValues(const EmulatorFeatureFlags::Flags& flags) {
  // Every part is length-prefixed, so different settings have different
  // values.
  std::string values = absl::StrCat(
      sizeof(flags), ":",
      std::string_view(reinterpret_cast<const char*>(&flags), sizeof(flags)));
  for (const absl::CommandLineFlag* flag : SettingsFlags()) {
    std::string value = flag->CurrentValue();
    absl::StrAppend(&values, ";", flag->Name(), "=", value.size(), ":",
                    value);
  }
  return values;
}

}  // namespace

SchemaCreateCache::Settings SchemaCreateCache::CurrentSettings() {
  EmulatorFeatureFlags::Snapshot snapshot =
      EmulatorFeatureFlags::instance().snapshot();
  return Settings{.generation = snapshot.generation,
                  .values = SettingsValues(snapshot.flags)};
}

bool SchemaCreateCache::SettingsUnchanged(const Settings& before) {
  Settings now = CurrentSettings();
  return now.generation == before.generation && now.values == before.values;
}

std::string SchemaCreateCache::Key(const SchemaChangeOperation& operation,
                                   const Settings& settings) {
  // Every part is length-prefixed, so different requests have different keys.
  std::string key = absl::StrCat(
      static_cast<int>(operation.database_dialect), ";",
      settings.values.size(), ":", settings.values, ";",
      operation.proto_descriptor_bytes.size(), ":",
      operation.proto_descriptor_bytes);
  for (const std::string& statement : operation.statements) {
    absl::StrAppend(&key, ";", statement.size(), ":", statement);
  }
  return key;
}

std::shared_ptr<const SchemaCreateCache::Entry> SchemaCreateCache::Lookup(
    const std::string& key) {
  absl::MutexLock lock(mu_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->second;
}

std::shared_ptr<const SchemaCreateCache::Entry> SchemaCreateCache::Peek(
    const std::string& key) const {
  absl::MutexLock lock(mu_);
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : it->second->second;
}

void SchemaCreateCache::Insert(const std::string& key,
                               std::shared_ptr<const Entry> entry) {
  absl::MutexLock lock(mu_);
  if (auto it = index_.find(key); it != index_.end()) {
    it->second->second = std::move(entry);
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }
  lru_.emplace_front(key, std::move(entry));
  index_[key] = lru_.begin();
  while (lru_.size() > static_cast<size_t>(capacity_)) {
    index_.erase(lru_.back().first);
    lru_.pop_back();
  }
}

void SchemaCreateCache::RecordCopy() {
  absl::MutexLock lock(mu_);
  ++copies_;
}

void SchemaCreateCache::RecordFallback() {
  absl::MutexLock lock(mu_);
  ++fallbacks_;
}

void SchemaCreateCache::RecordRejected() {
  absl::MutexLock lock(mu_);
  ++rejected_;
}

int64_t SchemaCreateCache::copies() const {
  absl::MutexLock lock(mu_);
  return copies_;
}

int64_t SchemaCreateCache::fallbacks() const {
  absl::MutexLock lock(mu_);
  return fallbacks_;
}

int64_t SchemaCreateCache::rejected() const {
  absl::MutexLock lock(mu_);
  return rejected_;
}

int64_t SchemaCreateCache::hits() const {
  absl::MutexLock lock(mu_);
  return hits_;
}

int64_t SchemaCreateCache::misses() const {
  absl::MutexLock lock(mu_);
  return misses_;
}

int SchemaCreateCache::size() const {
  absl::MutexLock lock(mu_);
  return lru_.size();
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
