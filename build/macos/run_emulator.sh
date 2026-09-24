#!/bin/bash
#
# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Runs the natively built emulator on macOS: the REST gateway, which starts
# emulator_main (gRPC) as its child. See docs/building-on-macos.md.
#
# Usage: build/macos/run_emulator.sh [--build] [gateway_main flags...]
#
#   --build   build //binaries:emulator_main and //binaries:gateway_main first
#
# Environment:
#   SPANNER_EMULATOR_HOST_NAME  address to listen on (default: localhost)
#   SPANNER_EMULATOR_GRPC_PORT  gRPC port (default: 9010)
#   SPANNER_EMULATOR_REST_PORT  REST port (default: 9020)
#   BAZEL_FLAGS                 flags selecting the build configuration
#                               (default: -c opt, as in the release image)

set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "run_emulator.sh is for macOS; on Linux use the Docker image." >&2
  exit 1
fi

cd "$(dirname "$0")/../.."

bazel_flags="${BAZEL_FLAGS:--c opt}"

if [[ "${1:-}" == "--build" ]]; then
  shift
  # shellcheck disable=SC2086
  bazel build ${bazel_flags} //binaries:emulator_main //binaries:gateway_main
fi

# shellcheck disable=SC2086
bin_dir="$(bazel info ${bazel_flags} bazel-bin 2>/dev/null)/binaries"
gateway="${bin_dir}/gateway_main_/gateway_main"
emulator="${bin_dir}/emulator_main"
for f in "${gateway}" "${emulator}"; do
  if [[ ! -x "${f}" ]]; then
    echo "${f} not found; run with --build first." >&2
    exit 1
  fi
done

# Stop emulator_main ourselves: the gateway exits on SIGINT but never kills its
# child (it releases the process before killing it), so a `kill` of the gateway
# alone leaves emulator_main running. Once the child exits, so does the gateway.
"${gateway}" \
  --hostname "${SPANNER_EMULATOR_HOST_NAME:-localhost}" \
  --grpc_port "${SPANNER_EMULATOR_GRPC_PORT:-9010}" \
  --http_port "${SPANNER_EMULATOR_REST_PORT:-9020}" \
  --grpc_binary "${emulator}" \
  "$@" &
gateway_pid=$!
# shellcheck disable=SC2329 # invoked by the trap below
stop() {
  pkill -TERM -P "${gateway_pid}" 2>/dev/null || true
  kill -INT "${gateway_pid}" 2>/dev/null || true
}
trap stop INT TERM HUP

status=0
# wait returns early when a trapped signal arrives; keep waiting for shutdown.
while kill -0 "${gateway_pid}" 2>/dev/null; do
  wait "${gateway_pid}" || status=$?
done
exit "${status}"
