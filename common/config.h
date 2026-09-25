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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CONFIG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_CONFIG_H_

#include <string>

namespace google {
namespace spanner {
namespace emulator {
namespace config {

// The address at which the emulator will serve gRPC requests.
std::string grpc_host_port();

// If true, gRPC requests and response messages are streamed to the INFO log.
bool should_log_requests();

// Returns true if fault injection is enabled.
bool fault_injection_enabled();

// If true, then queries that use NULL_FILTERED indexes will be answered.
//
// Note: The emulator cannot be used to validate the behavior of NULL_FILTERED
// indexes. Before disabling this check, please ensure that you have tested the
// queries that use null filtered indexes against production Cloud Spanner.
//
// Please consider using the query hint
// `@{spanner_emulator.disable_query_null_filtered_index_check=true}` to disable
// this check per query, instead of disabling this check for all the queries at
// once.
bool disable_query_null_filtered_index_check();

// The percentage (0-100) of read-write commits aborted at random, for testing
// application retry loops. Zero (the default) disables it.
int abort_current_transaction_probability();

void set_abort_current_transaction_probability(int probability);

// Returns true if primary key predicates narrow the key ranges that SQL
// statements read.
bool query_key_pushdown_enabled();

// Sets the query key pushdown flag (for tests).
void set_query_key_pushdown_enabled(bool enabled);

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_COMMON_FLAGS_H_
