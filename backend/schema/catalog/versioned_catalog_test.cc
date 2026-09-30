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

#include <memory>
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

}  // namespace
}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
