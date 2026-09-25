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

#include "backend/query/key_narrowing.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/function.h"
#include "googlesql/public/types/type.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "googlesql/resolved_ast/resolved_ast_visitor.h"
#include "googlesql/resolved_ast/resolved_node_kind.pb.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/queryable_column.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/table.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// Beyond this many point keys, a key column's IN list is not expanded.
constexpr int kMaxPointKeys = 1000;

struct Bound {
  googlesql::Value value;
  bool inclusive;
};

// What the conjuncts seen so far say about one key column.
struct ColumnConstraint {
  // Intersection of the equality and IN lists, if any.
  std::optional<std::vector<googlesql::Value>> points;
  std::optional<Bound> lower;
  std::optional<Bound> upper;
};

// Key columns whose SQL comparison agrees with the storage key order and
// equality. Floating point keys are excluded (-0.0 = 0.0 in SQL), as are
// extended types and commit timestamp columns (whose buffered values are
// placeholders).
bool IsNarrowableKeyColumn(const Column* column) {
  if (column->allows_commit_timestamp()) {
    return false;
  }
  switch (column->GetType()->kind()) {
    case googlesql::TYPE_INT64:
    case googlesql::TYPE_STRING:
    case googlesql::TYPE_BYTES:
    case googlesql::TYPE_BOOL:
    case googlesql::TYPE_DATE:
    case googlesql::TYPE_TIMESTAMP:
    case googlesql::TYPE_NUMERIC:
      return true;
    default:
      return false;
  }
}

bool Contains(const std::vector<googlesql::Value>& values,
              const googlesql::Value& value) {
  return std::any_of(values.begin(), values.end(),
                     [&](const googlesql::Value& v) { return v.Equals(value); });
}

void AddPoints(ColumnConstraint& constraint,
               std::vector<googlesql::Value> values) {
  if (!constraint.points.has_value()) {
    constraint.points = std::move(values);
    return;
  }
  std::vector<googlesql::Value> both;
  for (const googlesql::Value& value : *constraint.points) {
    if (Contains(values, value)) {
      both.push_back(value);
    }
  }
  constraint.points = std::move(both);
}

void AddLower(ColumnConstraint& constraint, Bound bound) {
  if (!constraint.lower.has_value() ||
      constraint.lower->value.LessThan(bound.value)) {
    constraint.lower = std::move(bound);
  } else if (constraint.lower->value.Equals(bound.value)) {
    constraint.lower->inclusive &= bound.inclusive;
  }
}

void AddUpper(ColumnConstraint& constraint, Bound bound) {
  if (!constraint.upper.has_value() ||
      bound.value.LessThan(constraint.upper->value)) {
    constraint.upper = std::move(bound);
  } else if (constraint.upper->value.Equals(bound.value)) {
    constraint.upper->inclusive &= bound.inclusive;
  }
}

bool WithinBounds(const ColumnConstraint& constraint,
                  const googlesql::Value& value) {
  if (constraint.lower.has_value()) {
    const Bound& lower = *constraint.lower;
    if (value.LessThan(lower.value) ||
        (!lower.inclusive && value.Equals(lower.value))) {
      return false;
    }
  }
  if (constraint.upper.has_value()) {
    const Bound& upper = *constraint.upper;
    if (upper.value.LessThan(value) ||
        (!upper.inclusive && value.Equals(upper.value))) {
      return false;
    }
  }
  return true;
}

// True if no value satisfies both bounds.
bool BoundsAreEmpty(const ColumnConstraint& constraint) {
  if (!constraint.lower.has_value() || !constraint.upper.has_value()) {
    return false;
  }
  const Bound& lower = *constraint.lower;
  const Bound& upper = *constraint.upper;
  if (upper.value.LessThan(lower.value)) {
    return true;
  }
  return lower.value.Equals(upper.value) &&
         !(lower.inclusive && upper.inclusive);
}

// A range over one key column after `prefix`, in key order. For a descending
// column the SQL upper bound comes first.
KeyRange MakeRange(const std::vector<googlesql::Value>& prefix,
                   const ColumnConstraint& constraint, bool descending) {
  const std::optional<Bound>& first =
      descending ? constraint.upper : constraint.lower;
  const std::optional<Bound>& last =
      descending ? constraint.lower : constraint.upper;
  // A closed endpoint that is just the prefix covers every key with that
  // prefix (KeyRange prefix semantics), i.e. the column is unbounded there.
  Key start(prefix);
  EndpointType start_type = EndpointType::kClosed;
  if (first.has_value()) {
    start.AddColumn(first->value);
    start_type = first->inclusive ? EndpointType::kClosed : EndpointType::kOpen;
  }
  Key limit(prefix);
  EndpointType limit_type = EndpointType::kClosed;
  if (last.has_value()) {
    limit.AddColumn(last->value);
    limit_type = last->inclusive ? EndpointType::kClosed : EndpointType::kOpen;
  }
  return KeyRange(start_type, start, limit_type, limit);
}

// Builds the key set implied by per-key-column constraints: equality/IN
// columns expand to key prefixes, the first range column bounds each prefix,
// and the rest of the key is free. Returns nullopt if the first key column is
// unconstrained.
std::optional<KeySet> BuildKeySet(
    const Table* table, const std::vector<ColumnConstraint>& constraints) {
  absl::Span<const KeyColumn* const> primary_key = table->primary_key();
  std::vector<std::vector<googlesql::Value>> prefixes = {{}};
  for (int i = 0; i < primary_key.size(); ++i) {
    const ColumnConstraint& constraint = constraints[i];
    if (constraint.points.has_value()) {
      std::vector<googlesql::Value> points;
      for (const googlesql::Value& value : *constraint.points) {
        if (WithinBounds(constraint, value) && !Contains(points, value)) {
          points.push_back(value);
        }
      }
      if (points.empty()) {
        // Contradictory predicates: no row matches.
        return KeySet();
      }
      if (prefixes.size() * points.size() > kMaxPointKeys) {
        break;
      }
      std::vector<std::vector<googlesql::Value>> extended;
      extended.reserve(prefixes.size() * points.size());
      for (const auto& prefix : prefixes) {
        for (const googlesql::Value& point : points) {
          extended.push_back(prefix);
          extended.back().push_back(point);
        }
      }
      prefixes = std::move(extended);
      continue;
    }
    if (constraint.lower.has_value() || constraint.upper.has_value()) {
      // Never emit an inverted range: contradictory bounds match no row.
      if (BoundsAreEmpty(constraint)) {
        return KeySet();
      }
      KeySet key_set;
      for (const auto& prefix : prefixes) {
        key_set.AddRange(
            MakeRange(prefix, constraint, primary_key[i]->is_descending()));
      }
      return key_set;
    }
    break;
  }
  if (prefixes.size() == 1 && prefixes[0].empty()) {
    return std::nullopt;
  }
  KeySet key_set;
  for (const auto& prefix : prefixes) {
    if (prefix.size() == primary_key.size()) {
      key_set.AddKey(Key(prefix));
    } else {
      key_set.AddRange(KeyRange::ClosedClosed(Key(prefix), Key(prefix)));
    }
  }
  return key_set;
}

// The backend column behind a googlesql catalog column, or null.
const Column* BackendColumn(const googlesql::Column* column) {
  const auto* queryable_column = dynamic_cast<const QueryableColumn*>(column);
  return queryable_column == nullptr ? nullptr
                                     : queryable_column->wrapped_column();
}

// Returns the position of `column` in `table`'s primary key, or -1.
int KeyPosition(const Table* table, const Column* column) {
  absl::Span<const KeyColumn* const> primary_key = table->primary_key();
  for (int i = 0; i < primary_key.size(); ++i) {
    if (primary_key[i]->column() == column) {
      return i;
    }
  }
  return -1;
}

// One table scan whose reads may be narrowed.
class ScanConstraints {
 public:
  // Returns nullopt if the scan's table is not a user table.
  static std::optional<ScanConstraints> Create(
      const googlesql::ResolvedTableScan* scan,
      const std::map<std::string, googlesql::Value>& parameters) {
    const std::vector<int>& column_index_list = scan->column_index_list();
    if (column_index_list.size() != scan->column_list().size()) {
      return std::nullopt;
    }
    const Table* table = nullptr;
    absl::flat_hash_map<int, int> key_position_by_column_id;
    for (int i = 0; i < scan->column_list().size(); ++i) {
      const Column* column =
          BackendColumn(scan->table()->GetColumn(column_index_list[i]));
      if (column == nullptr) {
        return std::nullopt;
      }
      table = column->table();
      int position = KeyPosition(table, column);
      if (position >= 0 && IsNarrowableKeyColumn(column)) {
        key_position_by_column_id[scan->column_list()[i].column_id()] =
            position;
      }
    }
    if (table == nullptr || table->owner_change_stream() != nullptr) {
      return std::nullopt;
    }
    return ScanConstraints(table, std::move(key_position_by_column_id),
                           parameters);
  }

  const Table* table() const { return table_; }

  // Adds what a WHERE expression's top-level conjuncts say about key columns.
  void AddFilter(const googlesql::ResolvedExpr* expr) {
    if (!expr->Is<googlesql::ResolvedFunctionCall>()) {
      return;
    }
    const auto* call = expr->GetAs<googlesql::ResolvedFunctionCall>();
    if (!call->function()->IsGoogleSQLBuiltin()) {
      return;
    }
    const std::string name = call->function()->FullName(
        /*include_group=*/false);
    const auto& args = call->argument_list();
    if (name == "$and") {
      for (const auto& arg : args) {
        AddFilter(arg.get());
      }
      return;
    }
    if (args.empty()) {
      return;
    }
    if (name == "$between" && args.size() == 3) {
      int position = KeyColumnRef(args[0].get());
      std::optional<googlesql::Value> lower =
          KeyValue(position, args[1].get());
      std::optional<googlesql::Value> upper =
          KeyValue(position, args[2].get());
      if (lower.has_value() && upper.has_value()) {
        AddLower(constraints_[position], {*lower, true});
        AddUpper(constraints_[position], {*upper, true});
      }
      return;
    }
    if (name == "$in" || name == "$in_array") {
      int position = KeyColumnRef(args[0].get());
      if (position < 0) {
        return;
      }
      std::vector<googlesql::Value> points;
      if (name == "$in_array") {
        if (args.size() != 2) {
          return;
        }
        std::optional<googlesql::Value> array = Constant(args[1].get());
        if (!array.has_value() || array->is_null() ||
            !array->type()->IsArray()) {
          return;
        }
        for (const googlesql::Value& element : array->elements()) {
          if (!AddPoint(position, element, points)) {
            return;
          }
        }
      } else {
        for (int i = 1; i < args.size(); ++i) {
          std::optional<googlesql::Value> element = Constant(args[i].get());
          if (!element.has_value() || !AddPoint(position, *element, points)) {
            return;
          }
        }
      }
      AddPoints(constraints_[position], std::move(points));
      return;
    }
    if (args.size() != 2) {
      return;
    }
    // Normalize to `column <op> value`.
    int position = KeyColumnRef(args[0].get());
    const googlesql::ResolvedExpr* value_expr = args[1].get();
    bool swapped = false;
    if (position < 0) {
      position = KeyColumnRef(args[1].get());
      value_expr = args[0].get();
      swapped = true;
    }
    std::optional<googlesql::Value> value = KeyValue(position, value_expr);
    if (!value.has_value()) {
      return;
    }
    ColumnConstraint& constraint = constraints_[position];
    if (name == "$equal") {
      AddPoints(constraint, {*value});
    } else if (name == "$less" || name == "$less_or_equal") {
      Bound bound{*value, name == "$less_or_equal"};
      swapped ? AddLower(constraint, bound) : AddUpper(constraint, bound);
    } else if (name == "$greater" || name == "$greater_or_equal") {
      Bound bound{*value, name == "$greater_or_equal"};
      swapped ? AddUpper(constraint, bound) : AddLower(constraint, bound);
    }
  }

  std::optional<KeySet> KeySetFromFilters() const {
    return BuildKeySet(table_, constraints_);
  }

  // The key set of the rows an INSERT ... VALUES statement inserts, or
  // nullopt if a key is not given as a literal or parameter.
  std::optional<KeySet> InsertedKeys(
      const googlesql::ResolvedInsertStmt* insert) const {
    int key_size = table_->primary_key().size();
    std::vector<int> value_index(key_size, -1);
    for (int i = 0; i < insert->insert_column_list().size(); ++i) {
      auto it = key_position_by_column_id_.find(
          insert->insert_column_list()[i].column_id());
      if (it != key_position_by_column_id_.end()) {
        value_index[it->second] = i;
      }
    }
    if (std::find(value_index.begin(), value_index.end(), -1) !=
        value_index.end()) {
      return std::nullopt;
    }
    KeySet key_set;
    for (const auto& row : insert->row_list()) {
      std::vector<googlesql::Value> key;
      for (int position = 0; position < key_size; ++position) {
        int index = value_index[position];
        if (index >= row->value_list().size()) {
          return std::nullopt;
        }
        std::optional<googlesql::Value> value =
            Constant(row->value_list()[index]->value());
        if (!value.has_value() ||
            !value->type()->Equals(
                table_->primary_key()[position]->column()->GetType())) {
          return std::nullopt;
        }
        key.push_back(*value);
      }
      key_set.AddKey(Key(std::move(key)));
    }
    return key_set;
  }

 private:
  ScanConstraints(const Table* table,
                  absl::flat_hash_map<int, int> key_position_by_column_id,
                  const std::map<std::string, googlesql::Value>& parameters)
      : table_(table),
        key_position_by_column_id_(std::move(key_position_by_column_id)),
        parameters_(parameters),
        constraints_(table->primary_key().size()) {}

  // The key position of a reference to one of this scan's narrowable key
  // columns, or -1.
  int KeyColumnRef(const googlesql::ResolvedExpr* expr) const {
    if (expr == nullptr || !expr->Is<googlesql::ResolvedColumnRef>()) {
      return -1;
    }
    const auto* ref = expr->GetAs<googlesql::ResolvedColumnRef>();
    if (ref->is_correlated()) {
      return -1;
    }
    auto it = key_position_by_column_id_.find(ref->column().column_id());
    return it == key_position_by_column_id_.end() ? -1 : it->second;
  }

  // The value of a literal or query parameter.
  std::optional<googlesql::Value> Constant(
      const googlesql::ResolvedExpr* expr) const {
    if (expr == nullptr) {
      return std::nullopt;
    }
    if (expr->Is<googlesql::ResolvedLiteral>()) {
      return expr->GetAs<googlesql::ResolvedLiteral>()->value();
    }
    if (expr->Is<googlesql::ResolvedParameter>()) {
      const std::string& name =
          expr->GetAs<googlesql::ResolvedParameter>()->name();
      if (name.empty()) {
        return std::nullopt;
      }
      auto it = parameters_.find(name);
      if (it != parameters_.end()) {
        return it->second;
      }
      for (const auto& [parameter_name, value] : parameters_) {
        if (absl::EqualsIgnoreCase(parameter_name, name)) {
          return value;
        }
      }
    }
    return std::nullopt;
  }

  // A non-NULL constant of the key column's type compared with the key column
  // at `position`.
  std::optional<googlesql::Value> KeyValue(
      int position, const googlesql::ResolvedExpr* expr) const {
    if (position < 0) {
      return std::nullopt;
    }
    std::optional<googlesql::Value> value = Constant(expr);
    if (!value.has_value() || value->is_null() ||
        !value->type()->Equals(
            table_->primary_key()[position]->column()->GetType())) {
      return std::nullopt;
    }
    return value;
  }

  // Adds an IN list element. NULL never matches and is skipped. Returns false
  // if the element's type differs from the column's.
  bool AddPoint(int position, const googlesql::Value& element,
                std::vector<googlesql::Value>& points) const {
    if (!element.type()->Equals(
            table_->primary_key()[position]->column()->GetType())) {
      return false;
    }
    if (!element.is_null()) {
      points.push_back(element);
    }
    return true;
  }

  const Table* table_;
  absl::flat_hash_map<int, int> key_position_by_column_id_;
  const std::map<std::string, googlesql::Value>& parameters_;
  std::vector<ColumnConstraint> constraints_;
};

// Collects the table scans of a statement.
class ScanCollector : public googlesql::ResolvedASTVisitor {
 public:
  absl::Status VisitResolvedTableScan(
      const googlesql::ResolvedTableScan* node) override {
    ++scan_count_[node->table()];
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedFilterScan(
      const googlesql::ResolvedFilterScan* node) override {
    if (node->input_scan() != nullptr &&
        node->input_scan()->Is<googlesql::ResolvedTableScan>()) {
      filtered_scans_.emplace_back(
          node->input_scan()->GetAs<googlesql::ResolvedTableScan>(),
          node->filter_expr());
    }
    return DefaultVisit(node);
  }

  absl::Status VisitResolvedTVFScan(
      const googlesql::ResolvedTVFScan* node) override {
    unsupported_ = true;
    return DefaultVisit(node);
  }

  absl::Status DefaultVisit(const googlesql::ResolvedNode* node) override {
    if (absl::StrContains(node->node_kind_string(), "Graph")) {
      unsupported_ = true;
    }
    return googlesql::ResolvedASTVisitor::DefaultVisit(node);
  }

  bool ScannedOnce(const googlesql::ResolvedTableScan* scan) const {
    auto it = scan_count_.find(scan->table());
    return it != scan_count_.end() && it->second == 1;
  }

  bool unsupported() const { return unsupported_; }

  const std::vector<std::pair<const googlesql::ResolvedTableScan*,
                              const googlesql::ResolvedExpr*>>&
  filtered_scans() const {
    return filtered_scans_;
  }

 private:
  absl::flat_hash_map<const googlesql::Table*, int> scan_count_;
  std::vector<std::pair<const googlesql::ResolvedTableScan*,
                        const googlesql::ResolvedExpr*>>
      filtered_scans_;
  bool unsupported_ = false;
};

}  // namespace

ScanKeySets ComputeScanKeySets(
    const googlesql::ResolvedStatement* statement,
    const std::map<std::string, googlesql::Value>& parameters,
    bool filter_pushdown) {
  ScanKeySets key_sets;
  ScanCollector collector;
  if (!statement->Accept(&collector).ok() || collector.unsupported()) {
    return key_sets;
  }

  auto narrow_by_filter = [&](const googlesql::ResolvedTableScan* scan,
                              const googlesql::ResolvedExpr* filter) {
    if (!filter_pushdown || scan == nullptr || filter == nullptr ||
        !collector.ScannedOnce(scan)) {
      return;
    }
    std::optional<ScanConstraints> constraints =
        ScanConstraints::Create(scan, parameters);
    if (!constraints.has_value()) {
      return;
    }
    constraints->AddFilter(filter);
    std::optional<KeySet> key_set = constraints->KeySetFromFilters();
    if (key_set.has_value()) {
      key_sets[scan->table()] = *std::move(key_set);
    }
  };

  switch (statement->node_kind()) {
    case googlesql::RESOLVED_INSERT_STMT: {
      const auto* insert = statement->GetAs<googlesql::ResolvedInsertStmt>();
      const googlesql::ResolvedTableScan* scan = insert->table_scan();
      if (scan != nullptr && insert->query() == nullptr &&
          !insert->row_list().empty() && collector.ScannedOnce(scan)) {
        std::optional<ScanConstraints> constraints =
            ScanConstraints::Create(scan, parameters);
        if (constraints.has_value()) {
          std::optional<KeySet> key_set = constraints->InsertedKeys(insert);
          if (key_set.has_value()) {
            key_sets[scan->table()] = *std::move(key_set);
          }
        }
      }
      break;
    }
    case googlesql::RESOLVED_UPDATE_STMT: {
      const auto* update = statement->GetAs<googlesql::ResolvedUpdateStmt>();
      if (update->from_scan() == nullptr) {
        narrow_by_filter(update->table_scan(), update->where_expr());
      }
      break;
    }
    case googlesql::RESOLVED_DELETE_STMT: {
      const auto* del = statement->GetAs<googlesql::ResolvedDeleteStmt>();
      narrow_by_filter(del->table_scan(), del->where_expr());
      break;
    }
    default:
      break;
  }

  for (const auto& [scan, filter] : collector.filtered_scans()) {
    narrow_by_filter(scan, filter);
  }
  return key_sets;
}

std::optional<KeySet> KeySetFromColumnFilters(
    const Table* table, const std::vector<const Column*>& scan_columns,
    const absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>&
        filter_map) {
  std::vector<ColumnConstraint> constraints(table->primary_key().size());
  for (const auto& [index, filter] : filter_map) {
    if (index < 0 || index >= scan_columns.size() || filter == nullptr) {
      continue;
    }
    const Column* column = scan_columns[index];
    int position = KeyPosition(table, column);
    if (position < 0 || !IsNarrowableKeyColumn(column)) {
      continue;
    }
    const googlesql::Type* type = column->GetType();
    ColumnConstraint& constraint = constraints[position];
    switch (filter->kind()) {
      case googlesql::ColumnFilter::kRange:
        if (filter->lower_bound().is_valid() &&
            filter->lower_bound().type()->Equals(type)) {
          AddLower(constraint, {filter->lower_bound(), true});
        }
        if (filter->upper_bound().is_valid() &&
            filter->upper_bound().type()->Equals(type)) {
          AddUpper(constraint, {filter->upper_bound(), true});
        }
        break;
      case googlesql::ColumnFilter::kInList: {
        std::vector<googlesql::Value> points;
        bool usable = true;
        for (const googlesql::Value& value : filter->in_list()) {
          if (!value.type()->Equals(type)) {
            usable = false;
            break;
          }
          points.push_back(value);
        }
        if (usable) {
          AddPoints(constraint, std::move(points));
        }
        break;
      }
      default:
        break;
    }
  }
  return BuildKeySet(table, constraints);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
