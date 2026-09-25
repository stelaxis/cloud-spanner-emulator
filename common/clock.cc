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
#include <functional>
#include <utility>

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {

Clock::Clock() : Clock([]() { return absl::Now(); }) {}

Clock::Clock(std::function<absl::Time()> system_now)
    : system_now_(std::move(system_now)),
      last_system_time_(SystemNowMicros()),
      last_dispensed_time_(last_system_time_) {}

absl::Time Clock::SystemNowMicros() const {
  return absl::FromUnixMicros(absl::ToUnixMicros(system_now_()));
}

absl::Time Clock::Now() {
  absl::MutexLock lock(mu_);

  // Follow the system clock, stepping by one microsecond only when it has not
  // passed the last value handed out. Timestamps can run ahead of the system
  // clock during a burst of calls within one microsecond, or after it steps
  // back, but only until it catches up: the lead does not accumulate. Adding
  // the elapsed system time to the last value instead let every step add up
  // to permanent drift, which read timestamps then waited out (upstream issue
  // #277).
  absl::Time now = SystemNowMicros();
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
