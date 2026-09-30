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

#include "common/heap_release.h"

#include "gtest/gtest.h"

namespace google {
namespace spanner {
namespace emulator {
namespace {

TEST(HeapReleaseTest, OneReleaseForAnyNumberOfRequests) {
  const HeapReleaseCounts before = GetHeapReleaseCounts();
  EXPECT_FALSE(ReleaseHeapIfRequested());

  RequestHeapRelease();
  RequestHeapRelease();
  RequestHeapRelease();
  EXPECT_TRUE(ReleaseHeapIfRequested());
  EXPECT_FALSE(ReleaseHeapIfRequested());

  const HeapReleaseCounts after = GetHeapReleaseCounts();
  EXPECT_EQ(after.requests, before.requests + 3);
  EXPECT_EQ(after.releases, before.releases + 1);
}

}  // namespace
}  // namespace emulator
}  // namespace spanner
}  // namespace google
