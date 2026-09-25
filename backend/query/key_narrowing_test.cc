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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "googlesql/public/value.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "backend/access/read.h"
#include "backend/database/database.h"
#include "backend/datamodel/key.h"
#include "backend/datamodel/key_range.h"
#include "backend/datamodel/key_set.h"
#include "backend/query/query_context.h"
#include "backend/query/query_engine.h"
#include "backend/transaction/options.h"
#include "backend/transaction/read_write_transaction.h"
#include "common/clock.h"
#include "common/config.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

using googlesql::values::Int64;
using googlesql::values::String;
using googlesql_base::testing::StatusIs;

// Records the ReadArgs a query issues, and forwards them.
class RecordingReader : public RowReader {
 public:
  explicit RecordingReader(RowReader* reader) : reader_(reader) {}

  absl::Status Read(const ReadArg& read_arg,
                    std::unique_ptr<RowCursor>* cursor) override {
    reads_.push_back(read_arg);
    return reader_->Read(read_arg, cursor);
  }

  // Returns the key sets read from `table`.
  std::vector<KeySet> KeySetsOf(const std::string& table) const {
    std::vector<KeySet> key_sets;
    for (const ReadArg& read : reads_) {
      if (read.table == table) key_sets.push_back(read.key_set);
    }
    return key_sets;
  }

 private:
  RowReader* reader_;
  std::vector<ReadArg> reads_;
};

bool IsAll(const KeySet& key_set) {
  return key_set.keys().empty() && key_set.ranges().size() == 1 &&
         key_set.ranges()[0] == KeyRange::All();
}

class KeyNarrowingTest : public testing::Test {
 protected:
  void SetUp() override {
    saved_pushdown_ = config::query_key_pushdown_enabled();
    config::set_query_key_pushdown_enabled(true);
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(
        db_, Database::Create(&clock_, "test-db",
                              SchemaChangeOperation{.statements = {
                                                        R"(
          CREATE TABLE T (
            k INT64 NOT NULL,
            v INT64,
            s STRING(MAX),
          ) PRIMARY KEY (k)
        )",
                                                        R"(
          CREATE TABLE C (
            a INT64 NOT NULL,
            b STRING(MAX) NOT NULL,
            v INT64,
          ) PRIMARY KEY (a, b DESC)
        )",
                                                        R"(
          CREATE TABLE Floats (
            f FLOAT64 NOT NULL,
            v INT64,
          ) PRIMARY KEY (f)
        )"}}));
    std::vector<std::string> seed;
    for (int k = 0; k < 20; ++k) {
      seed.push_back(absl::StrCat("INSERT INTO T (k, v, s) VALUES (", k, ", ",
                                  k * 10, ", 's", k % 7, "')"));
    }
    for (int a = 0; a < 4; ++a) {
      for (const char* b : {"a", "f", "m", "q", "z"}) {
        seed.push_back(absl::StrCat("INSERT INTO C (a, b, v) VALUES (", a,
                                    ", '", b, "', ", a, ")"));
      }
    }
    for (double f : {-1.5, 0.0, 0.5, 2.0}) {
      seed.push_back(absl::StrCat("INSERT INTO Floats (f, v) VALUES (", f, ", 1)"));
    }
    auto txn = NewTransaction();
    for (const std::string& sql : seed) {
      GOOGLESQL_ASSERT_OK(Execute(txn.get(), Query{.sql = sql}).status());
    }
    GOOGLESQL_ASSERT_OK(txn->Commit());
  }

  void TearDown() override {
    config::set_query_key_pushdown_enabled(saved_pushdown_);
  }

  std::unique_ptr<ReadWriteTransaction> NewTransaction() {
    return db_->CreateReadWriteTransaction(ReadWriteOptions(), RetryState())
        .value();
  }

  absl::StatusOr<QueryResult> Execute(ReadWriteTransaction* txn,
                                      const Query& query,
                                      RecordingReader* recorder = nullptr) {
    RowReader* reader = txn;
    if (recorder != nullptr) reader = recorder;
    return db_->query_engine()->ExecuteSql(
        query, QueryContext{.schema = txn->schema(),
                            .reader = reader,
                            .writer = txn,
                            .commit_timestamp_tracker =
                                txn->commit_timestamp_tracker(),
                            .allow_read_write_only_functions = true,
                            .is_read_only_txn = false});
  }

  // Runs `query` and returns the key sets it read from `table`.
  std::vector<KeySet> KeySetsRead(const Query& query,
                                  const std::string& table) {
    auto txn = NewTransaction();
    RecordingReader recorder(txn.get());
    absl::StatusOr<QueryResult> result = Execute(txn.get(), query, &recorder);
    EXPECT_TRUE(result.ok()) << result.status();
    if (result.ok() && result->rows != nullptr) {
      while (result->rows->Next()) {
      }
    }
    return recorder.KeySetsOf(table);
  }

  // Runs `query` and renders its rows (or error) as a string.
  std::string Run(const Query& query) {
    auto txn = NewTransaction();
    absl::StatusOr<QueryResult> result = Execute(txn.get(), query);
    if (!result.ok()) return result.status().ToString();
    std::vector<std::string> rows;
    if (result->rows != nullptr) {
      while (result->rows->Next()) {
        std::vector<std::string> values;
        for (int i = 0; i < result->rows->NumColumns(); ++i) {
          values.push_back(result->rows->ColumnValue(i).DebugString());
        }
        rows.push_back(absl::StrJoin(values, ","));
      }
    }
    return absl::StrCat(result->modified_row_count, ":",
                        absl::StrJoin(rows, ";"));
  }

  Clock clock_;
  std::unique_ptr<Database> db_;
  bool saved_pushdown_ = true;
};

TEST_F(KeyNarrowingTest, EqualityReadsOneKey) {
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "SELECT k, v FROM T WHERE k = @k",
            .declared_params = {{"k", Int64(3)}}},
      "T");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].keys().size(), 1);
  EXPECT_EQ(reads[0].keys()[0], Key({Int64(3)}));
  EXPECT_TRUE(reads[0].ranges().empty());
}

TEST_F(KeyNarrowingTest, StrictBoundsStayStrict) {
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "SELECT k FROM T WHERE k > 2 AND k < @hi AND v > 0",
            .declared_params = {{"hi", Int64(5)}}},
      "T");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].ranges().size(), 1);
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange::OpenOpen(Key({Int64(2)}), Key({Int64(5)})));
}

TEST_F(KeyNarrowingTest, InclusiveAndHalfOpenBounds) {
  std::vector<KeySet> reads =
      KeySetsRead(Query{.sql = "SELECT k FROM T WHERE 4 <= k AND k <= 9"}, "T");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].ranges().size(), 1);
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange::ClosedClosed(Key({Int64(4)}), Key({Int64(9)})));

  reads = KeySetsRead(Query{.sql = "SELECT k FROM T WHERE k >= 17"}, "T");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].ranges().size(), 1);
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange::ClosedClosed(Key({Int64(17)}), Key()));
}

TEST_F(KeyNarrowingTest, InListAndUnnest) {
  std::vector<KeySet> reads =
      KeySetsRead(Query{.sql = "SELECT k FROM T WHERE k IN (1, 3, 3)"}, "T");
  ASSERT_EQ(reads.size(), 1);
  EXPECT_EQ(reads[0].keys().size(), 2);

  reads = KeySetsRead(
      Query{.sql = "SELECT k FROM T WHERE k IN UNNEST(@ks)",
            .declared_params = {{"ks", googlesql::values::Int64Array(
                                           {5, 6, 7})}}},
      "T");
  ASSERT_EQ(reads.size(), 1);
  EXPECT_EQ(reads[0].keys().size(), 3);
}

TEST_F(KeyNarrowingTest, CompositeDescendingKey) {
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "SELECT a, b FROM C WHERE a = 1 AND b > 'f' AND b <= 'q'"},
      "C");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].ranges().size(), 1);
  // Descending `b`: the SQL upper bound comes first in key order.
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange(EndpointType::kClosed, Key({Int64(1), String("q")}),
                     EndpointType::kOpen, Key({Int64(1), String("f")})));

  reads = KeySetsRead(Query{.sql = "SELECT a, b FROM C WHERE a = 2"}, "C");
  ASSERT_EQ(reads.size(), 1);
  ASSERT_EQ(reads[0].ranges().size(), 1);
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange::ClosedClosed(Key({Int64(2)}), Key({Int64(2)})));
}

TEST_F(KeyNarrowingTest, UnusablePredicatesReadTheWholeTable) {
  for (const char* sql :
       {"SELECT k FROM T", "SELECT k FROM T WHERE v = 10",
        "SELECT k FROM T WHERE k = 1 OR k = 2",
        "SELECT k FROM T WHERE NOT (k = 1)", "SELECT k FROM T WHERE k + 1 = 2",
        "SELECT f FROM Floats WHERE f = 0.0"}) {
    std::string table = absl::StrContains(sql, "Floats") ? "Floats" : "T";
    std::vector<KeySet> reads = KeySetsRead(Query{.sql = sql}, table);
    ASSERT_EQ(reads.size(), 1) << sql;
    EXPECT_TRUE(IsAll(reads[0])) << sql << ": " << reads[0];
  }
}

TEST_F(KeyNarrowingTest, TableScannedTwiceUsesColumnFilters) {
  // Two scans of T: the statement analysis leaves them alone, and the
  // evaluator's column filters narrow each scan (inclusive bounds only).
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "SELECT x.k, y.k FROM T x, T y WHERE x.k = 1 AND y.k < 3"},
      "T");
  ASSERT_EQ(reads.size(), 2);
  for (const KeySet& key_set : reads) {
    EXPECT_FALSE(IsAll(key_set)) << key_set;
  }
}

TEST_F(KeyNarrowingTest, UpdateAndDeleteNarrow) {
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "UPDATE T SET v = 0 WHERE k = @k",
            .declared_params = {{"k", Int64(4)}}},
      "T");
  ASSERT_FALSE(reads.empty());
  ASSERT_EQ(reads[0].keys().size(), 1);
  EXPECT_EQ(reads[0].keys()[0], Key({Int64(4)}));

  reads = KeySetsRead(Query{.sql = "DELETE FROM T WHERE k >= 18"}, "T");
  ASSERT_FALSE(reads.empty());
  ASSERT_EQ(reads[0].ranges().size(), 1);
  EXPECT_EQ(reads[0].ranges()[0],
            KeyRange::ClosedClosed(Key({Int64(18)}), Key()));
}

TEST_F(KeyNarrowingTest, InsertReadsOnlyInsertedKeysEvenWithoutPushdown) {
  config::set_query_key_pushdown_enabled(false);
  std::vector<KeySet> reads = KeySetsRead(
      Query{.sql = "INSERT INTO T (k, v) VALUES (@k, 1), (101, 2)",
            .declared_params = {{"k", Int64(100)}}},
      "T");
  ASSERT_FALSE(reads.empty());
  EXPECT_EQ(reads[0].keys().size(), 2);

  reads = KeySetsRead(Query{.sql = "SELECT k FROM T WHERE k = 1"}, "T");
  ASSERT_EQ(reads.size(), 1);
  EXPECT_TRUE(IsAll(reads[0]));
}

// Pushdown must never change results: every query returns the same rows with
// and without it.
TEST_F(KeyNarrowingTest, PushdownPreservesResults) {
  const std::vector<Query> queries = {
      {.sql = "SELECT k, v FROM T WHERE k = 3"},
      {.sql = "SELECT k, v FROM T WHERE k = 300"},
      {.sql = "SELECT k FROM T WHERE k > 3 AND k < 7 ORDER BY k"},
      {.sql = "SELECT k FROM T WHERE k >= 3 AND k <= 7 ORDER BY k"},
      {.sql = "SELECT k FROM T WHERE k > 7 AND k < 3"},
      {.sql = "SELECT k FROM T WHERE k BETWEEN 5 AND 5"},
      {.sql = "SELECT k FROM T WHERE k BETWEEN 9 AND 2"},
      {.sql = "SELECT k FROM T WHERE k IN (1, 4, 99) ORDER BY k"},
      {.sql = "SELECT k FROM T WHERE k IN (1, 4) AND k > 1"},
      {.sql = "SELECT k FROM T WHERE k IN (1, 4) AND k = 2"},
      {.sql = "SELECT k FROM T WHERE k = 1 AND k = 1"},
      {.sql = "SELECT k FROM T WHERE k < 3 OR k > 17 ORDER BY k"},
      {.sql = "SELECT k FROM T WHERE k = @n", .declared_params = {{"n", googlesql::values::NullInt64()}}},
      {.sql = "SELECT k FROM T WHERE k IN UNNEST(@ks) ORDER BY k",
       .declared_params = {{"ks", googlesql::values::Int64Array({2, 3})}}},
      {.sql = "SELECT k FROM T WHERE k > 15 AND v > 100 ORDER BY k"},
      {.sql = "SELECT k FROM T WHERE s = 's3' AND k < 10 ORDER BY k"},
      {.sql = "SELECT COUNT(*) FROM T WHERE k <= 10"},
      {.sql = "SELECT k FROM T WHERE k > (SELECT MAX(a) FROM C) ORDER BY k"},
      {.sql = "SELECT x.k FROM T x JOIN T y ON x.k = y.k + 1 WHERE y.k = 4"},
      {.sql = "SELECT a, b FROM C WHERE a = 1 ORDER BY b"},
      {.sql = "SELECT a, b FROM C WHERE a = 1 AND b = 'm'"},
      {.sql = "SELECT a, b FROM C WHERE a IN (0, 3) AND b > 'f' ORDER BY a, b"},
      {.sql = "SELECT a, b FROM C WHERE a >= 2 AND b < 'q' ORDER BY a, b"},
      {.sql = "SELECT a, b FROM C WHERE b = 'z' ORDER BY a"},
      {.sql = "SELECT f FROM Floats WHERE f = 0 ORDER BY f"},
      {.sql = "SELECT f FROM Floats WHERE f > -1 ORDER BY f"},
      {.sql = "UPDATE T SET v = v + 1 WHERE k > 5 AND k < 9"},
      {.sql = "UPDATE T SET v = 0 WHERE k IN (2, 30)"},
      {.sql = "DELETE FROM T WHERE k >= 15"},
      {.sql = "DELETE FROM C WHERE a = 2 AND b <= 'm'"},
      {.sql = "UPDATE T SET v = 1 WHERE k = 3 THEN RETURN k, v"},
      {.sql = "INSERT INTO T (k, v) VALUES (3, 1)"},
      {.sql = "INSERT INTO T (k, v) VALUES (50, 1), (51, 2)"},
      {.sql = "INSERT OR UPDATE INTO T (k, v) VALUES (3, 7)"},
      {.sql = "INSERT INTO T (k, v) SELECT k + 100, v FROM T WHERE k < 3"},
  };
  for (const Query& query : queries) {
    config::set_query_key_pushdown_enabled(false);
    std::string without = Run(query);
    config::set_query_key_pushdown_enabled(true);
    std::string with = Run(query);
    EXPECT_EQ(with, without) << query.sql;
    // Only the duplicate INSERT is expected to fail.
    EXPECT_FALSE(absl::StrContains(without, "INVALID_ARGUMENT"))
        << query.sql << ": " << without;
  }
}

// With pushdown, SQL writes to different keys of one table don't conflict.
TEST_F(KeyNarrowingTest, DisjointDmlDoesNotConflict) {
  for (bool pushdown : {true, false}) {
    config::set_query_key_pushdown_enabled(pushdown);
    auto t1 = NewTransaction();
    auto t2 = NewTransaction();
    GOOGLESQL_ASSERT_OK(
        Execute(t1.get(), Query{.sql = "UPDATE T SET v = 1 WHERE k = 1"}));
    GOOGLESQL_ASSERT_OK(
        Execute(t2.get(), Query{.sql = "UPDATE T SET v = 2 WHERE k = 2"}));
    GOOGLESQL_EXPECT_OK(t1->Commit());
    if (pushdown) {
      GOOGLESQL_EXPECT_OK(t2->Commit());
    } else {
      // Without pushdown each UPDATE reads the whole table.
      EXPECT_THAT(t2->Commit(), StatusIs(absl::StatusCode::kAborted));
    }
  }
}

TEST_F(KeyNarrowingTest, OverlappingDmlConflicts) {
  auto t1 = NewTransaction();
  auto t2 = NewTransaction();
  GOOGLESQL_ASSERT_OK(Execute(
      t1.get(), Query{.sql = "UPDATE T SET v = 1 WHERE k >= 1 AND k < 5"}));
  GOOGLESQL_ASSERT_OK(
      Execute(t2.get(), Query{.sql = "UPDATE T SET v = 2 WHERE k = 4"}));
  GOOGLESQL_EXPECT_OK(t2->Commit());
  EXPECT_THAT(t1->Commit(), StatusIs(absl::StatusCode::kAborted));
}

// Contradictory bounds read nothing, and never an inverted range, including
// after the transaction buffered writes to the table.
TEST_F(KeyNarrowingTest, ContradictoryBoundsReadNothing) {
  auto txn = NewTransaction();
  GOOGLESQL_ASSERT_OK(
      Execute(txn.get(), Query{.sql = "INSERT INTO T (k) VALUES (300), (301)"}));
  for (const char* sql : {"SELECT k FROM T WHERE k > 7 AND k < 3",
                          "SELECT k FROM T WHERE k > 2 AND k < 2",
                          "SELECT k FROM T WHERE k BETWEEN 9 AND 2"}) {
    GOOGLESQL_ASSERT_OK_AND_ASSIGN(QueryResult result,
                                   Execute(txn.get(), Query{.sql = sql}));
    EXPECT_FALSE(result.rows->Next()) << sql;
  }
  std::vector<KeySet> reads =
      KeySetsRead(Query{.sql = "SELECT k FROM T WHERE k > 2 AND k < 2"}, "T");
  ASSERT_EQ(reads.size(), 1);
  EXPECT_TRUE(reads[0].keys().empty());
  EXPECT_TRUE(reads[0].ranges().empty());
}

TEST_F(KeyNarrowingTest, DisjointInsertsDoNotConflict) {
  config::set_query_key_pushdown_enabled(false);
  auto t1 = NewTransaction();
  auto t2 = NewTransaction();
  GOOGLESQL_ASSERT_OK(
      Execute(t1.get(), Query{.sql = "INSERT INTO T (k) VALUES (200)"}));
  GOOGLESQL_ASSERT_OK(
      Execute(t2.get(), Query{.sql = "INSERT INTO T (k) VALUES (201)"}));
  GOOGLESQL_EXPECT_OK(t1->Commit());
  GOOGLESQL_EXPECT_OK(t2->Commit());
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
