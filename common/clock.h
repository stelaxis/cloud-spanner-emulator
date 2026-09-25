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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CLOCK_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CLOCK_H_

#include <functional>

#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace google {
namespace spanner {
namespace emulator {

// Clock implements a strictly monotonic clock.
//
// This class provides two properties that are useful for the emulator:
//
// - The values returned from `Now()` are strictly monotonic - two calls to
//   `Now()` will never return the same value, and values returned from `Now()`
//   are always increasing. This allows us to use values from `Now()` as
//   commit timestamps.
//
// - The values returned from `Now()` are truncated to microsecond resolution.
//   This is to conform with Cloud Spanner's commit timestamps which also
//   operate at microsecond resolution.
//
// This class is thread safe.
class Clock {
 public:
  Clock();

  // Uses `system_now` instead of the system clock. For tests.
  explicit Clock(std::function<absl::Time()> system_now);

  // Returns the current time.
  absl::Time Now() ABSL_LOCKS_EXCLUDED(mu_);

  // Every later timestamp is after `floor`. For recovery (--data_dir).
  void AdvanceTo(absl::Time floor) ABSL_LOCKS_EXCLUDED(mu_);

  // With --data_dir, no timestamp handed out passes a durable lease, so that
  // after a crash the clock can restart above everything handed out before.
  // When Now() would pass the lease, `extend` must durably record and return
  // a lease at or after the timestamp it is given. The process aborts if it
  // cannot.
  using LeaseExtender =
      std::function<absl::StatusOr<absl::Time>(absl::Time needed)>;
  void SetLease(absl::Time lease, LeaseExtender extend)
      ABSL_LOCKS_EXCLUDED(mu_);

  // The current lease, or InfiniteFuture without one.
  absl::Time lease() ABSL_LOCKS_EXCLUDED(mu_);

 private:
  // Returns the system time at microsecond granularity.
  absl::Time SystemNowMicros() const;

  // The system clock; absl::Now unless a test replaces it.
  const std::function<absl::Time()> system_now_;

  // Mutex to guard state below.
  absl::Mutex mu_;

  // The last value we got from the system clock.
  absl::Time last_system_time_ ABSL_GUARDED_BY(mu_);

  // The last value we handed out in a call to Clock::Now().
  absl::Time last_dispensed_time_ ABSL_GUARDED_BY(mu_);

  absl::Time lease_ ABSL_GUARDED_BY(mu_) = absl::InfiniteFuture();
  LeaseExtender extend_lease_ ABSL_GUARDED_BY(mu_);
};

}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_COMMON_CLOCK_H_
