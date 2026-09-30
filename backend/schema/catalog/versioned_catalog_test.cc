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

#include "backend/schema/catalog/versioned_catalog.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/memory/memory.h"
#include "absl/time/time.h"
#include "backend/actions/manager.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {
namespace {

TEST(VersionedCatalogTest, FindSchemaAtTimeStamp) {
  VersionedCatalog catalog;

  absl::Time t1 = absl::Now();
  absl::Time t2 = t1 + absl::Seconds(1);
  absl::Time t3 = t2 + absl::Seconds(1);
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t1, std::make_unique<const Schema>()));
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t3, std::make_unique<const Schema>()));

  // Find schemas created at t1 and t3.
  const Schema* schema_t1 = catalog.GetSchema(t1);
  const Schema* schema_t3 = catalog.GetSchema(t3);
  EXPECT_NE(schema_t1, schema_t3);

  // Find a schema created at or before t2; expect the schema created at t1.
  EXPECT_EQ(catalog.GetSchema(t2), schema_t1);

  // Find a schema created at or before t4; expect the schema created at t3.
  absl::Time t4 = t3 + absl::Seconds(1);
  EXPECT_EQ(catalog.GetSchema(t4), schema_t3);
}

TEST(VersionedCatalog, FirstAndLastSchema) {
  VersionedCatalog catalog;
  absl::Time t1 = absl::Now();
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t1, std::make_unique<const Schema>()));

  // Find the default initial schema using absl::InfinitePast(). Expect it to be
  // different from the schema created at t1.
  EXPECT_NE(catalog.GetSchema(absl::InfinitePast()), catalog.GetSchema(t1));

  // Find the last schema using absl::InfiniteFuture(). Expect the schema
  // created at t1.
  EXPECT_EQ(catalog.GetSchema(absl::InfiniteFuture()), catalog.GetSchema(t1));
}

TEST(VersionedCatalogTest, InitialSchema) {
  absl::Time t1 = absl::Now();
  VersionedCatalog catalog(std::make_unique<const Schema>());
  absl::Time t0 = t1 - absl::Seconds(10);

  // Verify that the initial schema can be read with a timestamp in the past.
  EXPECT_EQ(catalog.GetSchema(t0), catalog.GetSchema(t1));
}

TEST(VersionedCatalogTest, FindFirstSchemaBeforeCreation) {
  VersionedCatalog catalog;

  absl::Time t1 = absl::Now();
  absl::Time t2 = t1 + absl::Seconds(1);
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t2, std::make_unique<const Schema>()));

  // Confirm that the schema created at t2 is not visible at t1.
  EXPECT_NE(catalog.GetSchema(t1), catalog.GetSchema(t2));
}

TEST(VersionedCatalogTest, AddSchemaWithSameOrEarlierCreationTime) {
  VersionedCatalog catalog;
  absl::Time t1 = absl::Now();
  absl::Time t2 = t1 + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t2, std::make_unique<const Schema>()));
  EXPECT_THAT(catalog.AddSchema(t2, std::make_unique<const Schema>()),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Failed to insert schema.*")));
  EXPECT_THAT(catalog.AddSchema(t1, std::make_unique<const Schema>()),
              googlesql_base::testing::StatusIs(
                  absl::StatusCode::kInternal,
                  testing::MatchesRegex(".*Failed to insert schema.*")));
}

TEST(VersionedCatalogTest, ExpiredSchemasThatCoverRetentionPeriodAreKept) {
  VersionedCatalog catalog;
  ActionManager action_manager;
  absl::Time t0 = absl::Now();
  absl::Time t1 = t0 + absl::Minutes(10);
  absl::Time t2 = t0 + absl::Minutes(40);
  absl::Time t3 = t0 + absl::Hours(1) + absl::Seconds(1);
  absl::Time t4 = t2 + absl::Hours(1) + absl::Seconds(1);

  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t0, std::make_unique<const Schema>()));
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t1, std::make_unique<const Schema>()));
  GOOGLESQL_EXPECT_OK(catalog.AddSchema(t2, std::make_unique<const Schema>()));

  catalog.RemoveExpiredSchemas(t3);
  // Verify that the schema created at t0 is not removed as it still covers the
  // retention period.
  EXPECT_NE(catalog.GetSchema(t0), catalog.GetSchema(absl::InfinitePast()));

  catalog.RemoveExpiredSchemas(t4);
  // Verify that the schema created at t0 is removed as it is no longer required
  // to cover the retention period.
  EXPECT_EQ(catalog.GetSchema(t0), catalog.GetSchema(absl::InfinitePast()));
}

TEST(VersionedCatalogTest, RemoveExpiredSchemasReturnsHowManyItRemoved) {
  VersionedCatalog catalog;
  absl::Time t0 = absl::Now();
  absl::Time t1 = t0 + absl::Minutes(10);
  absl::Time t2 = t0 + absl::Minutes(20);
  absl::Time t3 = t0 + absl::Minutes(30);
  for (absl::Time t : {t0, t1, t2, t3}) {
    GOOGLESQL_EXPECT_OK(catalog.AddSchema(t, std::make_unique<const Schema>()));
  }

  // Reads from a second after t2 on need the schema created at t2 and later.
  EXPECT_EQ(
      catalog.RemoveExpiredSchemas(t2 + absl::Hours(1) + absl::Seconds(1)), 2);
  EXPECT_THAT(catalog.SchemaTimestampsForTesting(),
              testing::ElementsAre(absl::InfinitePast(), t2, t3));
  EXPECT_EQ(
      catalog.RemoveExpiredSchemas(t2 + absl::Hours(1) + absl::Seconds(1)), 0);
}

TEST(VersionedCatalogTest, LookupsTellWhetherASweepRemovedTheirSchema) {
  VersionedCatalog catalog;
  absl::Time t1 = absl::Now();
  std::vector<absl::Time> t = {t1};
  for (int i = 1; i < 6; ++i) t.push_back(t1 + absl::Minutes(10 * i));
  for (absl::Time creation : t) {
    GOOGLESQL_EXPECT_OK(
        catalog.AddSchema(creation, std::make_unique<const Schema>()));
  }
  const Schema* first = catalog.GetSchema(absl::InfinitePast());
  bool swept = true;
  std::shared_ptr<const Schema> at_t0 = catalog.GetSchemaShared(t[0], &swept);
  EXPECT_FALSE(swept);

  // A schema change's removal is not reported.
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[1] + absl::Hours(1)), 1);
  EXPECT_EQ(catalog.GetSchemaShared(t[0], &swept).get(), first);
  EXPECT_FALSE(swept);

  // A sweep's is, from the oldest schema it removed to the one it kept.
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[3] + absl::Hours(1),
                                         /*swept=*/true),
            2);
  for (absl::Time time : {t[1], t[2], t[3] - absl::Microseconds(1)}) {
    EXPECT_EQ(catalog.GetSchemaShared(time, &swept).get(), first);
    EXPECT_TRUE(swept);
  }
  for (absl::Time time :
       {t[0] - absl::Seconds(1), t[0], t[1] - absl::Seconds(1)}) {
    EXPECT_EQ(catalog.GetSchemaShared(time, &swept).get(), first);
    EXPECT_FALSE(swept);
  }
  EXPECT_NE(catalog.GetSchemaShared(t[3], &swept).get(), first);
  EXPECT_FALSE(swept);

  // A later sweep extends the range.
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[5] + absl::Hours(1),
                                         /*swept=*/true),
            2);
  for (absl::Time time : {t[1], t[3], t[5] - absl::Microseconds(1)}) {
    catalog.GetSchemaShared(time, &swept);
    EXPECT_TRUE(swept);
  }
  catalog.GetSchemaShared(t[5], &swept);
  EXPECT_FALSE(swept);

  // A schema held elsewhere outlives its removal.
  EXPECT_EQ(at_t0.use_count(), 1);
}

// No schema is left in effect between two sweeps' removals, so lookups there
// report a sweep also where a schema change removed the schema.
TEST(VersionedCatalogTest, SchemaChangeRemovalsBetweenSweepsAreReported) {
  VersionedCatalog catalog;
  absl::Time t1 = absl::Now();
  std::vector<absl::Time> t = {t1};
  for (int i = 1; i < 8; ++i) t.push_back(t1 + absl::Minutes(10 * i));
  std::vector<const Schema*> schemas;
  for (absl::Time creation : t) {
    auto schema = std::make_unique<const Schema>();
    schemas.push_back(schema.get());
    GOOGLESQL_EXPECT_OK(catalog.AddSchema(creation, std::move(schema)));
  }
  const Schema* first = catalog.GetSchema(absl::InfinitePast());
  bool swept = true;

  // A schema change before any sweep, a sweep, a schema change, a sweep.
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[1] + absl::Hours(1)), 1);
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[3] + absl::Hours(1),
                                         /*swept=*/true),
            2);
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[5] + absl::Hours(1)), 2);
  // What a schema change removed after the latest sweep is not reported yet.
  for (absl::Time time : {t[3], t[5] - absl::Microseconds(1)}) {
    EXPECT_EQ(catalog.GetSchemaShared(time, &swept).get(), first);
    EXPECT_FALSE(swept);
  }
  EXPECT_EQ(catalog.RemoveExpiredSchemas(t[6] + absl::Hours(1),
                                         /*swept=*/true),
            1);
  EXPECT_THAT(catalog.SchemaTimestampsForTesting(),
              testing::ElementsAre(absl::InfinitePast(), t[6], t[7]));

  // Before the first sweep's removals, as upstream: the first schema.
  for (absl::Time time :
       {t[0] - absl::Seconds(1), t[0], t[1] - absl::Microseconds(1)}) {
    EXPECT_EQ(catalog.GetSchemaShared(time, &swept).get(), first);
    EXPECT_FALSE(swept);
  }
  // Swept, removed by the schema change between the sweeps, swept.
  for (absl::Time time :
       {t[1], t[2], t[3], t[4], t[5], t[6] - absl::Microseconds(1)}) {
    EXPECT_EQ(catalog.GetSchemaShared(time, &swept).get(), first);
    EXPECT_TRUE(swept);
  }
  // Kept.
  for (int i : {6, 7}) {
    EXPECT_EQ(catalog.GetSchemaShared(t[i], &swept).get(), schemas[i]);
    EXPECT_FALSE(swept);
  }
  EXPECT_EQ(catalog.GetSchemaShared(absl::InfiniteFuture(), &swept).get(),
            schemas[7]);
  EXPECT_FALSE(swept);
  EXPECT_THAT(catalog.SweptRangesForTesting(),
              testing::ElementsAre(testing::Pair(t[1], t[6])));
}

// However often sweeps and schema changes alternate, the catalog keeps one
// swept range.
TEST(VersionedCatalogTest, AlternatingSweepsAndSchemaChangesKeepOneSweptRange) {
  VersionedCatalog catalog;
  const Schema* first = catalog.GetSchema(absl::InfinitePast());
  const absl::Time t0 = absl::Now();
  auto t = [t0](int i) { return t0 + absl::Minutes(i); };
  GOOGLESQL_EXPECT_OK(
      catalog.AddSchema(t(0), std::make_unique<const Schema>()));

  // Each round adds a schema and removes the one before it: a sweep in odd
  // rounds, a schema change in even ones.
  constexpr int kRounds = 1000;
  for (int i = 1; i <= kRounds; ++i) {
    GOOGLESQL_EXPECT_OK(
        catalog.AddSchema(t(i), std::make_unique<const Schema>()));
    const bool sweep = i % 2 == 1;
    EXPECT_EQ(catalog.RemoveExpiredSchemas(t(i) + absl::Hours(1), sweep), 1);
    const std::vector<std::pair<absl::Time, absl::Time>> ranges =
        catalog.SweptRangesForTesting();
    ASSERT_EQ(ranges.size(), 1) << "after round " << i;
    EXPECT_EQ(ranges[0], std::make_pair(t(0), t(sweep ? i : i - 1)));
  }
  EXPECT_THAT(catalog.SchemaTimestampsForTesting(),
              testing::ElementsAre(absl::InfinitePast(), t(kRounds)));

  bool swept = true;
  EXPECT_EQ(catalog.GetSchemaShared(t(0) - absl::Seconds(1), &swept).get(),
            first);
  EXPECT_FALSE(swept);
  // Every schema before the last is removed; the last round was a schema
  // change after the latest sweep.
  for (int i = 0; i < kRounds; ++i) {
    EXPECT_EQ(catalog.GetSchemaShared(t(i), &swept).get(), first);
    EXPECT_EQ(swept, i < kRounds - 1) << "at t(" << i << ")";
  }
  EXPECT_NE(catalog.GetSchemaShared(t(kRounds), &swept).get(), first);
  EXPECT_FALSE(swept);
}

// The catalog's contract, kept as the history it follows from rather than in
// the catalog's own structures: every schema ever added, the removal that
// removed it, and which removals were sweeps that removed any.
class CatalogModel {
 public:
  void Add(absl::Time creation) { schemas_.push_back({creation}); }

  // Of the schemas after the first created at or before `timestamp` minus the
  // retention period (one hour), all but the newest go.
  int Remove(absl::Time timestamp, bool sweep) {
    ++removals_;
    std::vector<int> expired;
    for (int i = 1; i < schemas_.size(); ++i) {
      if (!Removed(i) && schemas_[i].creation <= timestamp - absl::Hours(1)) {
        expired.push_back(i);
      }
    }
    if (expired.size() < 2) return 0;
    expired.pop_back();
    for (int i : expired) {
      schemas_[i].removed_by = removals_;
      schemas_[i].by_sweep = sweep;
    }
    if (sweep) removing_sweeps_.push_back(removals_);
    return expired.size();
  }

  // The schema in effect at `timestamp`.
  int InEffect(absl::Time timestamp) const {
    int in_effect = 0;
    for (int i = 0; i < schemas_.size(); ++i) {
      if (schemas_[i].creation <= timestamp) in_effect = i;
    }
    return in_effect;
  }

  // What a lookup returns: the newest schema left created at or before
  // `timestamp`.
  int Returned(absl::Time timestamp) const {
    int returned = 0;
    for (int i = 0; i < schemas_.size(); ++i) {
      if (!Removed(i) && schemas_[i].creation <= timestamp) returned = i;
    }
    return returned;
  }

  // Whether a lookup reports a sweep: a sweep removed the schema in effect, or
  // a schema change did after one sweep removed schemas and before another.
  bool Swept(absl::Time timestamp) const {
    const ModelSchema& schema = schemas_[InEffect(timestamp)];
    if (schema.removed_by == 0) return false;
    if (schema.by_sweep) return true;
    return std::any_of(removing_sweeps_.begin(), removing_sweeps_.end(),
                       [&](int s) { return s < schema.removed_by; }) &&
           std::any_of(removing_sweeps_.begin(), removing_sweeps_.end(),
                       [&](int s) { return s > schema.removed_by; });
  }

  bool Removed(int i) const { return schemas_[i].removed_by != 0; }
  bool RemovedBySweep(int i) const { return schemas_[i].by_sweep; }
  absl::Time creation(int i) const { return schemas_[i].creation; }
  int size() const { return schemas_.size(); }

  std::vector<absl::Time> Kept() const {
    std::vector<absl::Time> kept;
    for (int i = 0; i < schemas_.size(); ++i) {
      if (!Removed(i)) kept.push_back(schemas_[i].creation);
    }
    return kept;
  }

 private:
  struct ModelSchema {
    absl::Time creation;
    int removed_by = 0;  // The removal that removed it, counted from 1.
    bool by_sweep = false;
  };
  std::vector<ModelSchema> schemas_ = {{absl::InfinitePast()}};
  int removals_ = 0;
  std::vector<int> removing_sweeps_;
};

// Random histories of schema changes (each adding a schema and removing
// expired ones, some at an earlier time, as after a lengthened retention
// period) and sweeps, checked after every step against CatalogModel: the
// schemas kept, what each removal removed, and at times in and around every
// schema, the schema a lookup returns and whether it reports a sweep. A
// quarter of the histories never sweep, as with the sweep disabled.
TEST(VersionedCatalogTest, RandomHistoriesMatchTheModel) {
  constexpr int kHistories = 2000;
  std::mt19937 gen(20261001);
  auto uniform = [&gen](int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(gen);
  };
  const absl::Time t0 = absl::FromUnixSeconds(1790000000);
  int mismatches = 0, lookups = 0, wrong_schema = 0;
  int between_sweeps = 0, never_swept_histories = 0;
  for (int h = 0; h < kHistories; ++h) {
    const bool sweeps = h % 4 != 0;
    if (!sweeps) ++never_swept_histories;
    VersionedCatalog catalog;
    CatalogModel model;
    std::vector<const Schema*> schemas = {
        catalog.GetSchema(absl::InfinitePast())};
    absl::Time now = t0 + absl::Minutes(uniform(0, 1000));
    const int steps = uniform(1, 40);
    for (int step = 0; step < steps; ++step) {
      if (sweeps && uniform(0, 99) < 55) {
        now += absl::Minutes(uniform(0, 180));
        EXPECT_EQ(catalog.RemoveExpiredSchemas(now, /*swept=*/true),
                  model.Remove(now, /*sweep=*/true));
      } else {
        now += absl::Minutes(uniform(1, 150));
        auto schema = std::make_unique<const Schema>();
        schemas.push_back(schema.get());
        ASSERT_TRUE(catalog.AddSchema(now, std::move(schema)).ok());
        model.Add(now);
        const absl::Time at =
            uniform(0, 3) == 0 ? now - absl::Minutes(uniform(0, 240)) : now;
        EXPECT_EQ(catalog.RemoveExpiredSchemas(at),
                  model.Remove(at, /*sweep=*/false));
      }
      ASSERT_EQ(catalog.SchemaTimestampsForTesting(), model.Kept())
          << "history " << h << ", step " << step;
      ASSERT_LE(catalog.SweptRangesForTesting().size(), 1);

      std::vector<absl::Time> times = {absl::InfinitePast(),
                                       absl::InfiniteFuture()};
      for (int i = 1; i < model.size(); ++i) {
        times.push_back(model.creation(i) - absl::Microseconds(1));
        times.push_back(model.creation(i));
        times.push_back(model.creation(i) + absl::Minutes(uniform(0, 149)));
      }
      for (absl::Time time : times) {
        ++lookups;
        bool swept;
        const Schema* got = catalog.GetSchemaShared(time, &swept).get();
        // Only kept schemas are alive, so only their addresses are unique.
        int got_index = -1;
        for (int i = 0; i < model.size(); ++i) {
          if (!model.Removed(i) && schemas[i] == got) got_index = i;
        }
        const int in_effect = model.InEffect(time);
        // Upstream answers from an older schema where a schema change removed
        // the one in effect; nothing else may.
        const bool upstream_removal =
            model.Removed(in_effect) && !model.Swept(time);
        if (!swept && got_index != in_effect && !upstream_removal) {
          ++wrong_schema;
        }
        if (model.Removed(in_effect) && !model.RemovedBySweep(in_effect) &&
            model.Swept(time)) {
          ++between_sweeps;
        }
        if (got_index != model.Returned(time) || swept != model.Swept(time)) {
          if (++mismatches <= 10) {
            ADD_FAILURE() << "history " << h << ", step " << step << ", at "
                          << time << ": got schema " << got_index
                          << (swept ? " swept" : "") << ", want "
                          << model.Returned(time)
                          << (model.Swept(time) ? " swept" : "");
          }
        }
      }
    }
  }
  std::cout << kHistories << " histories (" << never_swept_histories
            << " never sweeping), " << lookups << " lookups ("
            << between_sweeps
            << " where a schema change removed the schema between sweeps), "
            << mismatches << " mismatches, " << wrong_schema
            << " answered from the wrong schema\n";
  EXPECT_EQ(mismatches, 0);
  EXPECT_EQ(wrong_schema, 0);
  EXPECT_GT(between_sweeps, 0);
}

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
