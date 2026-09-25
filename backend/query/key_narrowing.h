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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_KEY_NARROWING_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_KEY_NARROWING_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "googlesql/public/catalog.h"
#include "googlesql/public/evaluator_table_iterator.h"
#include "googlesql/public/value.h"
#include "googlesql/resolved_ast/resolved_ast.h"
#include "absl/container/flat_hash_map.h"
#include "backend/datamodel/key_set.h"
#include "backend/schema/catalog/table.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

// Key sets that bound what a statement reads from a table: every row outside a
// table's key set is irrelevant to the statement's result. Keyed by the
// catalog's googlesql::Table (a QueryableTable).
using ScanKeySets = absl::flat_hash_map<const googlesql::Table*, KeySet>;

// Computes key sets for the table scans of `statement`. A read-write
// transaction records the key ranges it reads in its read set, so a narrower
// read means fewer conflicts at commit.
//
// * INSERT ... VALUES: the target table is read only to find existing rows
//   with the inserted keys, so its key set is those keys. Always on: the
//   Lean model's `fp (.dmlInsert ..)` is the inserted row.
// * With `filter_pushdown`, UPDATE/DELETE targets and scans directly under a
//   WHERE filter get the key set implied by the filter's top-level conjuncts
//   on primary key columns: `=`, `<`, `<=`, `>`, `>=`, BETWEEN, IN and
//   IN UNNEST against literals or parameters. Strict bounds stay strict.
//
// A table gets no key set (it is read in full) if the statement scans it more
// than once, if no usable predicate constrains its first key column, or if the
// statement contains graph or table-valued-function scans, which may read
// tables outside the resolved AST's table scans.
ScanKeySets ComputeScanKeySets(
    const googlesql::ResolvedStatement* statement,
    const std::map<std::string, googlesql::Value>& parameters,
    bool filter_pushdown);

// Returns the key set implied by GoogleSQL column filters (see
// googlesql::EvaluatorTableIterator::SetColumnFilterMap). `scan_columns[i]` is
// the table column behind index `i` of `filter_map`. Column filters lose the
// strictness of `<` and `>`, so ranges are closed. Returns nullopt if the
// filters do not constrain the first key column.
std::optional<KeySet> KeySetFromColumnFilters(
    const Table* table, const std::vector<const Column*>& scan_columns,
    const absl::flat_hash_map<int, std::unique_ptr<googlesql::ColumnFilter>>&
        filter_map);

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_BACKEND_QUERY_KEY_NARROWING_H_
