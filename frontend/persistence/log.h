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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_LOG_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_LOG_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "frontend/persistence/file_system.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

// The on-disk format version written. Files of versions from
// kOldestReadableFormatVersion up to it are read (version 1 stored timestamps
// as int64 nanoseconds); any other version is refused. Records are never
// appended to a file of an older version.
inline constexpr uint32_t kFormatVersion = 2;
inline constexpr uint32_t kOldestReadableFormatVersion = 1;

// What a data directory held when it was opened.
struct LogContents {
  // The published checkpoint's payload, if there is one.
  std::optional<std::string> checkpoint;
  // The first log sequence number (LSN) the checkpoint does not cover.
  uint64_t boundary = 0;
  // Payloads of the records with LSN >= boundary, in LSN order.
  std::vector<std::string> records;
  // Bytes of a torn final record that were discarded.
  uint64_t discarded_tail_bytes = 0;
};

// One physical write-ahead log per data directory, in segment files named by
// their first LSN, plus one checkpoint file.
//
// Files start with a header naming the file kind and format version. Each
// record is framed with its length, LSN and CRC32C checksums. A record is
// durable once Append returns. On Open, a damaged final record of the last
// segment is a torn write: it is discarded and truncated away before any
// append. Any other damage, a gap in LSNs or an unknown format version fails
// Open.
//
// A checkpoint is written to a temporary file, synced, renamed over the
// checkpoint file and made durable with a directory sync. Records below its
// boundary are skipped by Open, and their segments may then be removed.
class Log {
 public:
  // Opens (creating if needed) the log in `dir` and takes its lock file.
  static absl::StatusOr<std::unique_ptr<Log>> Open(FileSystem* fs,
                                                   const std::string& dir,
                                                   LogContents* contents);

  // Appends a record and syncs it; returns its LSN. After any failure the
  // log is broken: this and every later Append fail.
  absl::StatusOr<uint64_t> Append(absl::string_view payload)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Starts a new segment unless the current one is empty. Returns the LSN of
  // the next record, which a checkpoint captured now can use as its boundary.
  absl::StatusOr<uint64_t> StartSegment() ABSL_LOCKS_EXCLUDED(mu_);

  // Durably replaces the checkpoint.
  absl::Status PublishCheckpoint(uint64_t boundary, absl::string_view payload)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Removes the segments whose records are all below `boundary`, which must
  // not exceed the published checkpoint's boundary.
  absl::Status RemoveSegmentsBelow(uint64_t boundary) ABSL_LOCKS_EXCLUDED(mu_);

  // Bytes appended to the current segment.
  uint64_t CurrentSegmentBytes() ABSL_LOCKS_EXCLUDED(mu_);

  // OK unless an append failed.
  absl::Status health() ABSL_LOCKS_EXCLUDED(mu_);

 private:
  Log(FileSystem* fs, std::string dir) : fs_(fs), dir_(std::move(dir)) {}

  absl::Status CreateSegment(uint64_t first_lsn)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  std::string Path(absl::string_view name) const;

  FileSystem* const fs_;
  const std::string dir_;
  std::unique_ptr<FileLock> lock_;

  absl::Mutex mu_;
  // First LSN of each segment present, in order; the last one is current.
  std::vector<uint64_t> segments_ ABSL_GUARDED_BY(mu_);
  std::unique_ptr<WritableFile> current_ ABSL_GUARDED_BY(mu_);
  uint64_t next_lsn_ ABSL_GUARDED_BY(mu_) = 0;
  uint64_t current_bytes_ ABSL_GUARDED_BY(mu_) = 0;
  uint64_t checkpoint_boundary_ ABSL_GUARDED_BY(mu_) = 0;
  absl::Status broken_ ABSL_GUARDED_BY(mu_);
};

// Exposed for tests: the framing of one record.
std::string FrameRecord(uint64_t lsn, absl::string_view payload);

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_LOG_H_
