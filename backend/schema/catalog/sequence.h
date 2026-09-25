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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_SEQUENCE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_SEQUENCE_H_
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "googlesql/public/type.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/synchronization/mutex.h"
#include "backend/common/ids.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/updater/schema_validation_context.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

class Sequence : public SchemaNode {
 public:
  // Returns the name of this sequence.
  std::string Name() const { return name_; }
  const std::shared_ptr<Schema> schema() const { return schema_; }
  std::optional<int64_t> start_with_counter() const { return start_with_; }
  std::optional<int64_t> skip_range_min() const { return skip_range_min_; }
  std::optional<int64_t> skip_range_max() const { return skip_range_max_; }
  bool is_internal_use() const { return is_internal_use_; }

  // Returns a unique id of this sequence.
  const SequenceID id() const { return id_; }

  enum SequenceKind { BIT_REVERSED_POSITIVE = 0 };

  SequenceKind sequence_kind() const { return sequence_kind_; }

  std::string sequence_kind_name() const {
    if (sequence_kind_ == SequenceKind::BIT_REVERSED_POSITIVE) {
      return "BIT_REVERSED_POSITIVE";
    }
    return "INVALID";
  }

  bool created_from_syntax() const { return created_from_syntax_; }
  bool created_from_options() const { return created_from_options_; }
  bool use_default_sequence_kind_option() const {
    return use_default_sequence_kind_option_;
  }

  inline static absl::Mutex SequenceMutex;
  // A global map of sequence ids to their last values. This is used to
  // maintain the state of the sequence across multiple databases. Therefore,
  // the key should be a unique id for the sequence across all databases.
  inline static absl::flat_hash_map<std::string, int64_t> SequenceLastValues
      ABSL_GUARDED_BY(SequenceMutex);
  inline static std::function<absl::Status(const std::string&, int64_t)>
      reservation_hook_ ABSL_GUARDED_BY(SequenceMutex);
  inline static absl::flat_hash_map<std::string, int64_t> reservation_ends_
      ABSL_GUARDED_BY(SequenceMutex);

  // With --data_dir, a counter value is handed out only below a durable
  // reservation end, which the hook makes durable before returning. After a
  // restart the counter resumes at the reservation end, so no value is handed
  // out twice (the rest of the reserved range is skipped).
  using ReservationHook =
      std::function<absl::Status(const std::string& sequence_id, int64_t end)>;
  static void SetReservationHook(ReservationHook hook)
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Resumes a recovered sequence at its durable reservation end.
  static void RestoreReservation(const std::string& sequence_id, int64_t end)
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // The durable reservation end of every sequence that has one.
  static absl::flat_hash_map<std::string, int64_t> ReservationEnds()
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Returns the next sequence value according to the sequence kind.
  absl::StatusOr<googlesql::Value> GetNextSequenceValue() const
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Returns the internal current counter of the sequence.
  googlesql::Value GetInternalSequenceState() const
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Reset the sequence's last value to the schema's current start_with_.
  void ResetSequenceLastValue() const ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Remove the sequence from the last values map.
  void RemoveSequenceFromLastValuesMap() const
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // Forgets the counter and reservation of the sequence with this ID.
  static void ForgetState(const std::string& sequence_id)
      ABSL_LOCKS_EXCLUDED(SequenceMutex);

  // SchemaNode interface implementation.
  // ------------------------------------
  std::optional<SchemaNameInfo> GetSchemaNameInfo() const override {
    return SchemaNameInfo{.name = name_, .kind = "Sequence", .global = true};
  }
  absl::Status Validate(SchemaValidationContext* context) const override;
  absl::Status ValidateUpdate(const SchemaNode* old,
                              SchemaValidationContext* context) const override;
  std::string DebugString() const override;
  class Builder;
  class Editor;

 private:
  friend class SequenceValidator;

  using ValidationFn =
      std::function<absl::Status(const Sequence*, SchemaValidationContext*)>;
  using UpdateValidationFn = std::function<absl::Status(
      const Sequence*, const Sequence*, SchemaValidationContext*)>;

  // Constructors are private and only friend classes are able to build.
  Sequence(const ValidationFn& validate,
           const UpdateValidationFn& validate_update)
      : validate_(validate), validate_update_(validate_update) {}

  Sequence(const Sequence&) = default;

  std::unique_ptr<SchemaNode> ShallowClone() const override {
    return absl::WrapUnique(new Sequence(*this));
  }

  absl::Status DeepClone(SchemaGraphEditor* editor,
                         const SchemaNode* orig) override;
  // Validation delegates.
  const ValidationFn validate_;
  const UpdateValidationFn validate_update_;

  // Name of this sequence.
  std::string name_;
  // Whether this sequence is used internally, e.g., by identity columns.
  bool is_internal_use_ = false;

  // A globally unique ID for identifying this sequence across all databases.
  SequenceID id_;

  std::shared_ptr<Schema> schema_ = nullptr;

  SequenceKind sequence_kind_;
  std::optional<int64_t> start_with_;
  std::optional<int64_t> skip_range_min_;
  std::optional<int64_t> skip_range_max_;
  bool created_from_syntax_ = false;
  bool created_from_options_ = false;
  bool use_default_sequence_kind_option_ = false;
};

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_SCHEMA_CATALOG_SEQUENCE_H_
