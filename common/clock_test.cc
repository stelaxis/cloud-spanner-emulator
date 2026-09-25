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

#include "common/clock.h"

#include <functional>

#include "absl/time/time.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "googlesql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

TEST(Clock, ClockReturnsIncreasingValues) {
  Clock clock;
  absl::Time t1 = clock.Now();
  absl::Time t2 = clock.Now();

  EXPECT_GT(t2, t1);
}

TEST(Clock, ClockReturnsValuesAtMicrosecondGranularity) {
  Clock clock;
  absl::Time t1 = clock.Now();
  EXPECT_EQ(t1, absl::FromUnixMicros(absl::ToUnixMicros(t1)));
}

// A system clock the test moves by hand.
class FakeSystemClock {
 public:
  explicit FakeSystemClock(absl::Time now) : now_(now) {}
  std::function<absl::Time()> AsFunction() {
    return [this]() { return now_; };
  }
  void Set(absl::Time now) { now_ = now; }

 private:
  absl::Time now_;
};

const absl::Time kStart = absl::FromUnixMicros(1'000'000'000'000'000);

// Many calls within one system microsecond stay strictly increasing, and once
// the system clock passes the values handed out, the clock returns to it: the
// lead does not accumulate (upstream issue #277).
TEST(Clock, BurstWithinOneMicrosecondRejoinsTheSystemClock) {
  FakeSystemClock system(kStart);
  Clock clock(system.AsFunction());
  absl::Time last = clock.Now();
  for (int i = 0; i < 1000; ++i) {
    absl::Time now = clock.Now();
    EXPECT_GT(now, last);
    last = now;
  }
  // The first call already stepped past the construction-time value.
  EXPECT_EQ(last, kStart + absl::Microseconds(1001));

  // The system clock has not caught up yet: keep stepping.
  system.Set(kStart + absl::Microseconds(500));
  EXPECT_EQ(clock.Now(), kStart + absl::Microseconds(1002));

  // Once it has, follow it exactly.
  system.Set(kStart + absl::Milliseconds(5));
  EXPECT_EQ(clock.Now(), kStart + absl::Milliseconds(5));
  system.Set(kStart + absl::Milliseconds(7));
  EXPECT_EQ(clock.Now(), kStart + absl::Milliseconds(7));
}

// A backward step of the system clock never makes timestamps go backwards.
TEST(Clock, BackwardSystemClockStepKeepsTimestampsIncreasing) {
  FakeSystemClock system(kStart);
  Clock clock(system.AsFunction());
  absl::Time before = clock.Now();
  system.Set(kStart - absl::Seconds(10));
  absl::Time last = before;
  for (int i = 0; i < 100; ++i) {
    absl::Time now = clock.Now();
    EXPECT_GT(now, last);
    last = now;
  }
  EXPECT_EQ(last, before + absl::Microseconds(100));
  system.Set(kStart + absl::Seconds(1));
  EXPECT_EQ(clock.Now(), kStart + absl::Seconds(1));
}

// Values are truncated to microseconds.
TEST(Clock, TruncatesTheSystemClockToMicroseconds) {
  FakeSystemClock system(kStart + absl::Nanoseconds(1999));
  Clock clock(system.AsFunction());
  system.Set(kStart + absl::Microseconds(5) + absl::Nanoseconds(999));
  EXPECT_EQ(clock.Now(), kStart + absl::Microseconds(5));
}

}  // namespace

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
