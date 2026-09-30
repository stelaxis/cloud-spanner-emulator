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

#include "frontend/server/memory_reclaimer.h"

#include <algorithm>
#include <cstdint>

#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "common/config.h"
#include "common/heap_release.h"
#include "frontend/collections/database_manager.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

namespace {

// When a part with this interval is next due.
absl::Time NextDue(absl::Duration interval) {
  return interval > absl::ZeroDuration() ? absl::Now() + interval
                                         : absl::InfiniteFuture();
}

}  // namespace

MemoryReclaimer::MemoryReclaimer(DatabaseManager* database_manager)
    : database_manager_(database_manager),
      schema_version_gc_interval_(config::schema_version_gc_interval()),
      heap_release_interval_(config::heap_release_interval()),
      thread_([this] { Run(); }) {}

MemoryReclaimer::~MemoryReclaimer() {
  {
    absl::MutexLock lock(mu_);
    stopping_ = true;
  }
  thread_.join();
}

void MemoryReclaimer::RemoveExpiredSchemas() {
  if (database_manager_->RemoveExpiredSchemas() > 0) {
    RequestHeapRelease();
  }
}

void MemoryReclaimer::set_intervals_for_testing(
    absl::Duration schema_version_gc, absl::Duration heap_release) {
  absl::MutexLock lock(mu_);
  schema_version_gc_interval_ = schema_version_gc;
  heap_release_interval_ = heap_release;
  ++generation_;
}

void MemoryReclaimer::Run() {
  absl::Time next_gc, next_release;
  {
    absl::MutexLock lock(mu_);
    next_gc = NextDue(schema_version_gc_interval_);
    next_release = NextDue(heap_release_interval_);
  }
  while (true) {
    bool gc, release;
    {
      absl::MutexLock lock(mu_);
      const int64_t generation = generation_;
      auto woken = [this, generation]() ABSL_NO_THREAD_SAFETY_ANALYSIS {
        return stopping_ || generation_ != generation;
      };
      // Woken early only to stop or to take new intervals.
      if (mu_.AwaitWithDeadline(absl::Condition(&woken),
                                std::min(next_gc, next_release))) {
        if (stopping_) {
          return;
        }
        next_gc = NextDue(schema_version_gc_interval_);
        next_release = NextDue(heap_release_interval_);
        continue;
      }
      const absl::Time now = absl::Now();
      gc = now >= next_gc;
      release = now >= next_release;
      if (gc) next_gc = NextDue(schema_version_gc_interval_);
      if (release) next_release = NextDue(heap_release_interval_);
    }
    if (gc) RemoveExpiredSchemas();
    if (release) ReleaseHeapIfRequested();
  }
}

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
