//
// Copyright 2026 Google LLC
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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_SETTINGS_GENERATION_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_SETTINGS_GENERATION_H_

#include <atomic>
#include <cstdint>

namespace google {
namespace spanner {
namespace emulator {

namespace settings_generation_internal {
inline std::atomic<int64_t> generation{0};
}  // namespace settings_generation_internal

// Counts changes to the settings that schema changes read: the emulator
// feature flags, and the command-line flags defined in this repository that
// DDL processing reads. Each change bumps it after the new value is visible.
//
// A result computed from those settings may be reused only if the settings'
// values are the same after the computation as before, and the generation
// did not move. The values alone miss a change that is undone before the
// computation ends.
inline int64_t SettingsGeneration() {
  return settings_generation_internal::generation.load(
      std::memory_order_acquire);
}

inline void BumpSettingsGeneration() {
  settings_generation_internal::generation.fetch_add(1,
                                                     std::memory_order_acq_rel);
}

}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_SETTINGS_GENERATION_H_
