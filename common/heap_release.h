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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_HEAP_RELEASE_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_HEAP_RELEASE_H_

#include <cstdint>

namespace google {
namespace spanner {
namespace emulator {

// glibc keeps freed memory in its per-thread arenas for reuse, so the process
// stays as large as its busiest moment. Work that frees a lot asks for the free
// memory to be returned to the operating system, and a background thread
// returns it (frontend::MemoryReclaimer): no request waits for it.

// Asks for the free heap memory to be returned to the operating system.
// Cheap: it only marks the request.
void RequestHeapRelease();

// If RequestHeapRelease was called since the last release, returns the free
// heap memory to the operating system (glibc's malloc_trim) and returns true:
// one release for any number of requests. Elsewhere than glibc, the release
// does nothing, but is still counted.
bool ReleaseHeapIfRequested();

// Calls of RequestHeapRelease, and releases, so far. For tests.
struct HeapReleaseCounts {
  int64_t requests = 0;
  int64_t releases = 0;
};
HeapReleaseCounts GetHeapReleaseCounts();

}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_HEAP_RELEASE_H_
