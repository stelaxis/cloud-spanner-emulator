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

#include <algorithm>

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {

namespace {

// Returns the current time at microsecond granularity.
absl::Time NowMicros() {
  return absl::FromUnixMicros(absl::ToUnixMicros(absl::Now()));
}

}  // namespace

Clock::Clock()
    : last_system_time_(NowMicros()), last_dispensed_time_(last_system_time_) {}

absl::Time Clock::Now() {
  absl::MutexLock lock(mu_);

  // Track the system clock and step by one microsecond only when it has not
  // advanced. Adding the elapsed system time to the last dispensed time would
  // let every +1us step accumulate as permanent drift ahead of the system
  // clock, which read timestamps then wait out (upstream issue #277).
  absl::Time now = NowMicros();
  if (now <= last_dispensed_time_) {
    last_dispensed_time_ += absl::Microseconds(1);
  } else {
    last_dispensed_time_ = now;
  }
  last_system_time_ = now;

  return last_dispensed_time_;
}

}  // namespace emulator
}  // namespace spanner
}  // namespace google
