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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_MEMORY_RECLAIMER_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_MEMORY_RECLAIMER_H_

#include <cstdint>
#include <functional>
#include <thread>  // NOLINT

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "frontend/collections/database_manager.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {

// Frees memory in the background, so that no request waits for it. Every
// config::schema_version_gc_interval() it removes the expired schema versions
// of every database, and every config::heap_release_interval() it returns the
// free heap memory to the operating system if anything asked for that
// (ReleaseHeapIfRequested). Intervals are in real time, from the end of the
// previous sweep or release; a zero interval disables its part.
class MemoryReclaimer {
 public:
  // Starts the thread.
  explicit MemoryReclaimer(DatabaseManager* database_manager);

  // Stops and joins the thread.
  ~MemoryReclaimer();

  // Removes the expired schema versions of every database, and asks for the
  // heap to be released if it removed any.
  void RemoveExpiredSchemas() ABSL_LOCKS_EXCLUDED(mu_);

  // Makes the thread use these intervals from now on. For tests.
  void set_intervals_for_testing(absl::Duration schema_version_gc,
                                 absl::Duration heap_release)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Runs `before_sweep` at the start of every RemoveExpiredSchemas, and
  // `after_release` after every release the thread makes. For tests.
  void set_hooks_for_testing(std::function<void()> before_sweep,
                             std::function<void()> after_release)
      ABSL_LOCKS_EXCLUDED(mu_);

 private:
  void Run() ABSL_LOCKS_EXCLUDED(mu_);

  DatabaseManager* const database_manager_;

  absl::Mutex mu_;
  bool stopping_ ABSL_GUARDED_BY(mu_) = false;
  absl::Duration schema_version_gc_interval_ ABSL_GUARDED_BY(mu_);
  absl::Duration heap_release_interval_ ABSL_GUARDED_BY(mu_);
  // Bumped by set_intervals_for_testing, to wake the thread.
  int64_t generation_ ABSL_GUARDED_BY(mu_) = 0;
  std::function<void()> before_sweep_hook_ ABSL_GUARDED_BY(mu_);
  std::function<void()> after_release_hook_ ABSL_GUARDED_BY(mu_);

  // Declared last: it starts after, and stops before, everything above.
  std::thread thread_;
};

}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_SERVER_MEMORY_RECLAIMER_H_
