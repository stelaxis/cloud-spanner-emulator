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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_FILE_SYSTEM_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_FILE_SYSTEM_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

// A file open for appending. Appended bytes are durable only after Sync.
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual absl::Status Append(absl::string_view data) = 0;
  virtual absl::Status Sync() = 0;
};

// Released when destroyed.
class FileLock {
 public:
  virtual ~FileLock() = default;
};

// The file operations the log needs. Paths are absolute. A directory entry
// (create, rename, remove) is durable only after SyncDir of its directory.
class FileSystem {
 public:
  virtual ~FileSystem() = default;

  // Creates `dir` and its parents; OK if it exists.
  virtual absl::Status CreateDir(const std::string& dir) = 0;

  // Returns the names (not paths) of the entries in `dir`.
  virtual absl::StatusOr<std::vector<std::string>> ListDir(
      const std::string& dir) = 0;

  // Returns NOT_FOUND if `path` does not exist.
  virtual absl::StatusOr<std::string> ReadFile(const std::string& path) = 0;

  // Creates `path` empty, replacing any existing file, or with `truncate`
  // false, opens it and appends at its end.
  virtual absl::StatusOr<std::unique_ptr<WritableFile>> OpenForAppend(
      const std::string& path, bool truncate) = 0;

  // Shrinks `path` to `size` bytes and syncs it.
  virtual absl::Status Truncate(const std::string& path, uint64_t size) = 0;

  virtual absl::Status Rename(const std::string& from,
                              const std::string& to) = 0;
  virtual absl::Status Remove(const std::string& path) = 0;
  virtual absl::Status SyncDir(const std::string& dir) = 0;

  // Takes an exclusive lock on `path` (created if missing), held until the
  // returned object is destroyed. FAILED_PRECONDITION if another holder has
  // it, including another process.
  virtual absl::StatusOr<std::unique_ptr<FileLock>> Lock(
      const std::string& path) = 0;
};

// The real file system. Sync is fdatasync on Linux and fsync on macOS, which
// reaches the drive but not necessarily its platters (see F_FULLFSYNC).
FileSystem* PosixFileSystem();

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_FILE_SYSTEM_H_
