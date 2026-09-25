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

#ifndef THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MEM_FILE_SYSTEM_H_
#define THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MEM_FILE_SYSTEM_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "frontend/persistence/file_system.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

// An in-memory file system that models what survives a machine crash: file
// bytes survive only up to their last Sync, and directory entries (creates,
// renames, removes) only as of their directory's last SyncDir. Crash() drops
// everything else, optionally keeping some unsynced appended bytes to model a
// torn write. A hook can fail any operation. For tests.
class MemFileSystem : public FileSystem {
 public:
  enum class Op {
    kAppend,
    kSync,
    kTruncate,
    kRename,
    kRemove,
    kSyncDir,
    kOpen
  };

  // Called before each operation; a non-OK status fails it without effect.
  using Hook = std::function<absl::Status(Op op, const std::string& path)>;

  void SetHook(Hook hook) ABSL_LOCKS_EXCLUDED(mu_);

  // Loses everything not yet durable. Each file keeps its synced bytes plus
  // the first `keep_unsynced_bytes` of its unsynced tail.
  void Crash(uint64_t keep_unsynced_bytes = 0) ABSL_LOCKS_EXCLUDED(mu_);

  // Replaces the contents of `path` (live and durable), e.g. to corrupt it.
  void Overwrite(const std::string& path, const std::string& contents)
      ABSL_LOCKS_EXCLUDED(mu_);

  // Live contents of `path`, or NOT_FOUND.
  absl::StatusOr<std::string> Contents(const std::string& path)
      ABSL_LOCKS_EXCLUDED(mu_);

  absl::Status CreateDir(const std::string& dir) override;
  absl::StatusOr<std::vector<std::string>> ListDir(
      const std::string& dir) override ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<std::string> ReadFile(const std::string& path) override
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<std::unique_ptr<WritableFile>> OpenForAppend(
      const std::string& path, bool truncate) override ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status Truncate(const std::string& path, uint64_t size) override
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status Rename(const std::string& from, const std::string& to) override
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status Remove(const std::string& path) override
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::Status SyncDir(const std::string& dir) override
      ABSL_LOCKS_EXCLUDED(mu_);
  absl::StatusOr<std::unique_ptr<FileLock>> Lock(
      const std::string& path) override ABSL_LOCKS_EXCLUDED(mu_);

 private:
  friend class MemWritableFile;
  friend class MemFileLock;

  struct Node {
    std::string data;
    std::string synced;
  };

  absl::Status RunHook(Op op, const std::string& path)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);

  absl::Mutex mu_;
  Hook hook_ ABSL_GUARDED_BY(mu_);
  // Directory entries as a process sees them, and as of the last SyncDir.
  std::map<std::string, std::shared_ptr<Node>> live_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, std::shared_ptr<Node>> durable_ ABSL_GUARDED_BY(mu_);
  std::map<std::string, bool> locked_ ABSL_GUARDED_BY(mu_);
};

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google

#endif  // THIRD_PARTY_CLOUD_SPANNER_EMULATOR_FRONTEND_PERSISTENCE_MEM_FILE_SYSTEM_H_
