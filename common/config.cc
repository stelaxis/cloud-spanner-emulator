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

#include "common/config.h"

#include <string>

#include "absl/flags/flag.h"

ABSL_FLAG(std::string, host_port, "localhost:10007",
          "Emulator host IP and port that serves Cloud Spanner gRPC requests.");

ABSL_FLAG(bool, log_requests, false,
          "If true, gRPC request and response messages are streamed to the "
          "INFO log. This switch is intended for emulator debugging.");

ABSL_FLAG(
    bool, enable_fault_injection, false,
    "If true, the emulator will inject faults to allow testing application "
    "error handling behavior. For instance, transaction Commits may be aborted "
    "to facilitate application abort-retry testing.");

ABSL_FLAG(bool, disable_query_null_filtered_index_check, false,
          "If true, then queries that use NULL_FILTERED indexes will be "
          "answered. Please test all queries using null filtered indexes "
          "against production Cloud Spanner before disabling this check."
          "\n"
          "Please consider using the query hint "
          "`@{spanner_emulator.disable_query_null_filtered_index_check=true}` "
          "to disable this check per query, instead of disabling this check "
          "for all the queries at once.");

ABSL_FLAG(
    int, abort_current_transaction_probability, 0,
    "The percentage (0-100) of read-write transaction commits that the "
    "emulator aborts at random, to test application abort-retry loops. "
    "Read-write transactions run concurrently and abort only on a conflict, "
    "so this is the only source of random aborts. Zero disables it.");

ABSL_FLAG(
    bool, enable_query_key_pushdown, true,
    "If true, primary key predicates in SQL queries and DML (equality, IN and "
    "range comparisons with literals or parameters) narrow the key ranges a "
    "statement reads, and so the ranges its transaction validates at commit. "
    "If false, every query and UPDATE/DELETE reads its tables in full.");

ABSL_FLAG(int, ephemeral_session_idle_timeout_seconds, 120,
          "Seconds an ephemeral session (a session created with the label "
          "emulator-ephemeral=true, which has its own copy of its database) "
          "may go without a request before the emulator deletes it and its "
          "copy. Must be positive.");

ABSL_FLAG(int, max_ephemeral_sessions, 256,
          "The most ephemeral sessions, each with its own copy of its "
          "database, open at once. Creating another fails with "
          "RESOURCE_EXHAUSTED. Copies do not count towards "
          "--override_max_databases_per_instance.");

namespace google {
namespace spanner {
namespace emulator {
namespace config {

std::string grpc_host_port() { return absl::GetFlag(FLAGS_host_port); }

bool should_log_requests() { return absl::GetFlag(FLAGS_log_requests); }

bool fault_injection_enabled() {
  return absl::GetFlag(FLAGS_enable_fault_injection);
}

bool disable_query_null_filtered_index_check() {
  return absl::GetFlag(FLAGS_disable_query_null_filtered_index_check);
}

int abort_current_transaction_probability() {
  return absl::GetFlag(FLAGS_abort_current_transaction_probability);
}

void set_abort_current_transaction_probability(int probability) {
  absl::SetFlag(&FLAGS_abort_current_transaction_probability, probability);
}

bool query_key_pushdown_enabled() {
  return absl::GetFlag(FLAGS_enable_query_key_pushdown);
}

void set_query_key_pushdown_enabled(bool enabled) {
  absl::SetFlag(&FLAGS_enable_query_key_pushdown, enabled);
}

absl::Duration ephemeral_session_idle_timeout() {
  return absl::Seconds(
      absl::GetFlag(FLAGS_ephemeral_session_idle_timeout_seconds));
}

void set_ephemeral_session_idle_timeout(absl::Duration timeout) {
  absl::SetFlag(&FLAGS_ephemeral_session_idle_timeout_seconds,
                absl::ToInt64Seconds(timeout));
}

int max_ephemeral_sessions() {
  return absl::GetFlag(FLAGS_max_ephemeral_sessions);
}

void set_max_ephemeral_sessions(int max_sessions) {
  absl::SetFlag(&FLAGS_max_ephemeral_sessions, max_sessions);
}

}  // namespace config
}  // namespace emulator
}  // namespace spanner
}  // namespace google
