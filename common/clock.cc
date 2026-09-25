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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
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

  if (last_dispensed_time_ > lease_) {
    absl::StatusOr<absl::Time> lease = extend_lease_(last_dispensed_time_);
    if (!lease.ok() || *lease < last_dispensed_time_) {
      std::fprintf(stderr, "Cannot extend the durable clock lease: %s\n",
                   lease.status().ToString().c_str());
      std::abort();
    }
    lease_ = *lease;
  }
  return last_dispensed_time_;
}

void Clock::AdvanceTo(absl::Time floor) {
  absl::MutexLock lock(mu_);
  if (last_dispensed_time_ < floor) last_dispensed_time_ = floor;
}

void Clock::SetLease(absl::Time lease, LeaseExtender extend,
                     absl::Time latest_coverable) {
  absl::MutexLock lock(mu_);
  lease_ = lease;
  extend_lease_ = std::move(extend);
  latest_coverable_ = latest_coverable;
}

absl::Status Clock::CheckCoverable(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  return CheckCoverableLocked(timestamp);
}

absl::Status Clock::CheckCoverableLocked(absl::Time timestamp) const {
  if (timestamp <= latest_coverable_) return absl::OkStatus();
  return absl::InvalidArgumentError(
      absl::StrCat("Timestamp ", absl::FormatTime(timestamp),
                   " is too far in the future: with --data_dir the latest is ",
                   absl::FormatTime(latest_coverable_)));
}

absl::Status Clock::CoverWithLease(absl::Time timestamp) {
  absl::MutexLock lock(mu_);
  if (!extend_lease_ || timestamp <= lease_) return absl::OkStatus();
  if (absl::Status status = CheckCoverableLocked(timestamp); !status.ok()) {
    return status;
  }
  absl::StatusOr<absl::Time> lease = extend_lease_(timestamp);
  if (!lease.ok()) return lease.status();
  lease_ = std::max(lease_, *lease);
  return absl::OkStatus();
}

absl::Time Clock::lease() {
  absl::MutexLock lock(mu_);
  return lease_;
}

}  // namespace emulator
}  // namespace spanner
}  // namespace google
