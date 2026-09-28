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

#include "frontend/collections/operation_manager.h"

#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/strings/match.h"
#include "absl/time/clock.h"
#include "frontend/entities/database.h"
#include "frontend/entities/operation.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

class OperationManagerTest : public testing::Test {
 protected:
  OperationManager* manager() { return &manager_; }

 private:
  OperationManager manager_;
};

TEST_F(OperationManagerTest, CreatesNewOperationWithUserSpecifiedID) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> operation,
      manager()->CreateOperation("projects/123/instances/456", "789"));
  google::longrunning::Operation operation_pb;
  operation->ToProto(&operation_pb);
  EXPECT_EQ("projects/123/instances/456/operations/789", operation_pb.name());
}

TEST_F(OperationManagerTest, CreatesNewOperationWithSystemGeneratedId) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> operation,
      manager()->CreateOperation("projects/123/instances/456", ""));
  google::longrunning::Operation operation_pb;
  operation->ToProto(&operation_pb);
  EXPECT_TRUE(absl::StartsWith(operation_pb.name(),
                               "projects/123/instances/456/operations/_auto"));
}

TEST_F(OperationManagerTest, FailsToCreateOperationWithExistingURI) {
  GOOGLESQL_ASSERT_OK(manager()->CreateOperation("projects/123/instances/456", "789"));
  EXPECT_THAT(manager()->CreateOperation("projects/123/instances/456", "789"),
              googlesql_base::testing::StatusIs(absl::StatusCode::kAlreadyExists));
}

TEST_F(OperationManagerTest, GetExistingOperation) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> expected,
      manager()->CreateOperation("projects/123/instances/456", "789"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> actual,
      manager()->GetOperation("projects/123/instances/456/operations/789"));
  EXPECT_EQ(expected, actual);
}

TEST_F(OperationManagerTest, CannotGetNonExistingOperation) {
  EXPECT_THAT(
      manager()->GetOperation("projects/123/instances/456/operations/789"),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(OperationManagerTest, DeletesAnExistingOperation) {
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> expected,
      manager()->CreateOperation("projects/123/instances/456", "789"));
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<Operation> actual,
      manager()->GetOperation("projects/123/instances/456/operations/789"));
  EXPECT_EQ(expected, actual);

  GOOGLESQL_ASSERT_OK(
      manager()->DeleteOperation("projects/123/instances/456/operations/789"));
  EXPECT_THAT(
      manager()->GetOperation("projects/123/instances/456/operations/789"),
      googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(OperationManagerTest, DeletesANonExistingOperation) {
  GOOGLESQL_ASSERT_OK(
      manager()->DeleteOperation("projects/123/instances/456/operations/789"));
}

TEST_F(OperationManagerTest, ListsOperations) {
  // Create a set of operations.
  std::string instance_uri = "projects/test-project/instances/test-instance";
  const int kNumOperations = 5;
  for (int i = 0; i < kNumOperations; ++i) {
    GOOGLESQL_ASSERT_OK(manager()->CreateOperation(instance_uri, absl::StrCat(i)));
  }

  // Expect that they are returned in order.
  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::shared_ptr<Operation>> operations,
                       manager()->ListOperations(instance_uri));
  EXPECT_EQ(kNumOperations, operations.size());
  for (int i = 0; i < kNumOperations; ++i) {
    google::longrunning::Operation operation_pb;
    operations[i]->ToProto(&operation_pb);
    EXPECT_EQ(absl::StrCat(instance_uri, "/operations/", i),
              operation_pb.name());
  }
}

TEST_F(OperationManagerTest, ListsOperationsWithSimilarInstanceURI) {
  const std::string kInstanceURIa =
      absl::StrCat("projects/test-project/instances/test-instance-a");
  const std::string kInstanceURIb =
      absl::StrCat("projects/test-project/instances/test-instance-b");
  GOOGLESQL_ASSERT_OK(manager()->CreateOperation(kInstanceURIa, "1"));
  GOOGLESQL_ASSERT_OK(manager()->CreateOperation(kInstanceURIb, "1"));

  GOOGLESQL_ASSERT_OK_AND_ASSIGN(std::vector<std::shared_ptr<Operation>> operations,
                       manager()->ListOperations(
                           "projects/test-project/instances/test-instance-a"));
  EXPECT_EQ(operations.size(), 1);
  google::longrunning::Operation operation_pb;
  operations[0]->ToProto(&operation_pb);
  EXPECT_EQ("projects/test-project/instances/test-instance-a/operations/1",
            operation_pb.name());
}

TEST_F(OperationManagerTest, DeleteDatabaseOperationsKeepsOtherDatabases) {
  const std::string uri = "projects/1/instances/2/databases/db";
  auto database = std::make_shared<Database>(uri, nullptr, absl::Now());
  // Another database with a similar URI, and one re-created under the same.
  auto similar = std::make_shared<Database>(uri + "2", nullptr, absl::Now());
  auto recreated = std::make_shared<Database>(uri, nullptr, absl::Now());
  GOOGLESQL_ASSERT_OK(manager()->CreateDatabaseOperation(database, "a"));
  GOOGLESQL_ASSERT_OK(manager()->CreateDatabaseOperation(database, ""));
  GOOGLESQL_ASSERT_OK(manager()->CreateDatabaseOperation(similar, "a"));

  manager()->DeleteDatabaseOperations(recreated);
  EXPECT_THAT(manager()->ListOperations(uri + "/operations/"),
              googlesql_base::testing::IsOkAndHolds(testing::SizeIs(2)));
  manager()->DeleteDatabaseOperations(database);

  EXPECT_THAT(manager()->ListOperations(uri + "/operations/"),
              googlesql_base::testing::IsOkAndHolds(testing::IsEmpty()));
  GOOGLESQL_EXPECT_OK(manager()->GetOperation(uri + "2/operations/a"));
}

TEST_F(OperationManagerTest, RefusesOperationsOfDroppedDatabase) {
  const std::string uri = "projects/1/instances/2/databases/db";
  auto database = std::make_shared<Database>(uri, nullptr, absl::Now());
  database->MarkDropped();

  EXPECT_THAT(manager()->CreateDatabaseOperation(database, "a"),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
  EXPECT_THAT(manager()->GetOperation(uri + "/operations/a"),
              googlesql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
