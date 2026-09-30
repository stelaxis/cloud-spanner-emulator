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

#include <atomic>
#include <cstdint>
#include <cstdio>  // Defines __GLIBC__ with glibc.

#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#if defined(__GLIBC__)
#include <malloc.h>
#include <unistd.h>
#endif

namespace google {
namespace spanner {
namespace emulator {

namespace {

std::atomic<bool> requested = false;
std::atomic<int64_t> request_count = 0;
std::atomic<int64_t> release_count = 0;

#if defined(__GLIBC__)
double ResidentMiB() {
  long pages = 0, resident = 0;
  FILE* statm = std::fopen("/proc/self/statm", "r");
  if (statm == nullptr) return 0;
  if (std::fscanf(statm, "%ld %ld", &pages, &resident) != 2) resident = 0;
  std::fclose(statm);
  return static_cast<double>(resident) * sysconf(_SC_PAGESIZE) / (1 << 20);
}
#endif

void ReleaseHeap() {
#if defined(__GLIBC__)
  const double resident_before = ResidentMiB();
  const absl::Time start = absl::Now();
  malloc_trim(0);
  const absl::Duration took = absl::Now() - start;
  const struct mallinfo2 info = mallinfo2();
  ABSL_LOG(INFO) << absl::StrFormat(
      "Returned free heap memory to the OS in %.1f ms: resident %.1f MiB -> "
      "%.1f MiB, heap in use %.1f MiB",
      absl::ToDoubleMilliseconds(took), resident_before, ResidentMiB(),
      static_cast<double>(info.uordblks + info.hblkhd) / (1 << 20));
#endif
}

}  // namespace

void RequestHeapRelease() {
  request_count.fetch_add(1, std::memory_order_relaxed);
  requested.store(true);
}

bool ReleaseHeapIfRequested() {
  // Cleared first: memory freed while releasing is released next time.
  if (!requested.exchange(false)) {
    return false;
  }
  ReleaseHeap();
  release_count.fetch_add(1, std::memory_order_relaxed);
  return true;
}

HeapReleaseCounts GetHeapReleaseCounts() {
  return HeapReleaseCounts{.requests = request_count.load(),
                           .releases = release_count.load()};
}

}  // namespace emulator
}  // namespace spanner
}  // namespace google
