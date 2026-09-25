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

#include "frontend/persistence/mem_file_system.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

namespace {

std::string DirOf(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

}  // namespace

class MemWritableFile : public WritableFile {
 public:
  MemWritableFile(MemFileSystem* fs, std::string path,
                  std::shared_ptr<MemFileSystem::Node> node)
      : fs_(fs), path_(std::move(path)), node_(std::move(node)) {}

  absl::Status Append(absl::string_view data) override {
    absl::MutexLock lock(fs_->mu_);
    auto status = fs_->RunHook(MemFileSystem::Op::kAppend, path_);
    if (!status.ok()) return status;
    node_->data.append(data);
    return absl::OkStatus();
  }

  absl::Status Sync() override {
    absl::MutexLock lock(fs_->mu_);
    auto status = fs_->RunHook(MemFileSystem::Op::kSync, path_);
    if (!status.ok()) return status;
    node_->synced = node_->data;
    return absl::OkStatus();
  }

 private:
  MemFileSystem* fs_;
  const std::string path_;
  std::shared_ptr<MemFileSystem::Node> node_;
};

class MemFileLock : public FileLock {
 public:
  MemFileLock(MemFileSystem* fs, std::string path)
      : fs_(fs), path_(std::move(path)) {}
  ~MemFileLock() override {
    absl::MutexLock lock(fs_->mu_);
    fs_->locked_.erase(path_);
  }

 private:
  MemFileSystem* fs_;
  const std::string path_;
};

void MemFileSystem::SetHook(Hook hook) {
  absl::MutexLock lock(mu_);
  hook_ = std::move(hook);
}

absl::Status MemFileSystem::RunHook(Op op, const std::string& path) {
  return hook_ ? hook_(op, path) : absl::OkStatus();
}

void MemFileSystem::Crash(uint64_t keep_unsynced_bytes) {
  absl::MutexLock lock(mu_);
  for (auto& [path, node] : durable_) {
    std::string kept = node->synced;
    if (node->data.size() > node->synced.size() &&
        absl::StartsWith(node->data, node->synced)) {
      kept.append(node->data.substr(
          node->synced.size(),
          std::min<uint64_t>(keep_unsynced_bytes,
                             node->data.size() - node->synced.size())));
    }
    node->data = kept;
    node->synced = kept;
  }
  live_ = durable_;
  locked_.clear();
}

void MemFileSystem::Overwrite(const std::string& path,
                              const std::string& contents) {
  absl::MutexLock lock(mu_);
  auto node = std::make_shared<Node>(Node{contents, contents});
  live_[path] = node;
  durable_[path] = node;
}

absl::StatusOr<std::string> MemFileSystem::Contents(const std::string& path) {
  absl::MutexLock lock(mu_);
  auto it = live_.find(path);
  if (it == live_.end()) return absl::NotFoundError(path);
  return it->second->data;
}

absl::Status MemFileSystem::CreateDir(const std::string& dir) {
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> MemFileSystem::ListDir(
    const std::string& dir) {
  absl::MutexLock lock(mu_);
  std::vector<std::string> names;
  for (const auto& [path, node] : live_) {
    if (DirOf(path) == dir) names.push_back(path.substr(dir.size() + 1));
  }
  return names;
}

absl::StatusOr<std::string> MemFileSystem::ReadFile(const std::string& path) {
  return Contents(path);
}

absl::StatusOr<std::unique_ptr<WritableFile>> MemFileSystem::OpenForAppend(
    const std::string& path, bool truncate) {
  absl::MutexLock lock(mu_);
  auto status = RunHook(Op::kOpen, path);
  if (!status.ok()) return status;
  std::shared_ptr<Node>& node = live_[path];
  if (node == nullptr || truncate) {
    // A new inode: the durable entry, if any, keeps the old one until SyncDir.
    node = std::make_shared<Node>();
  }
  return std::make_unique<MemWritableFile>(this, path, node);
}

absl::Status MemFileSystem::Truncate(const std::string& path, uint64_t size) {
  absl::MutexLock lock(mu_);
  auto status = RunHook(Op::kTruncate, path);
  if (!status.ok()) return status;
  auto it = live_.find(path);
  if (it == live_.end()) return absl::NotFoundError(path);
  it->second->data.resize(std::min<uint64_t>(size, it->second->data.size()));
  it->second->synced = it->second->data;
  return absl::OkStatus();
}

absl::Status MemFileSystem::Rename(const std::string& from,
                                   const std::string& to) {
  absl::MutexLock lock(mu_);
  auto status = RunHook(Op::kRename, from);
  if (!status.ok()) return status;
  auto it = live_.find(from);
  if (it == live_.end()) return absl::NotFoundError(from);
  live_[to] = it->second;
  live_.erase(from);
  return absl::OkStatus();
}

absl::Status MemFileSystem::Remove(const std::string& path) {
  absl::MutexLock lock(mu_);
  auto status = RunHook(Op::kRemove, path);
  if (!status.ok()) return status;
  if (live_.erase(path) == 0) return absl::NotFoundError(path);
  return absl::OkStatus();
}

absl::Status MemFileSystem::SyncDir(const std::string& dir) {
  absl::MutexLock lock(mu_);
  auto status = RunHook(Op::kSyncDir, dir);
  if (!status.ok()) return status;
  for (auto it = durable_.begin(); it != durable_.end();) {
    it = DirOf(it->first) == dir ? durable_.erase(it) : std::next(it);
  }
  for (const auto& [path, node] : live_) {
    if (DirOf(path) == dir) durable_[path] = node;
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<FileLock>> MemFileSystem::Lock(
    const std::string& path) {
  absl::MutexLock lock(mu_);
  if (!locked_.emplace(path, true).second) {
    return absl::FailedPreconditionError(
        absl::StrCat(path, " is locked by another emulator process"));
  }
  return std::make_unique<MemFileLock>(this, path);
}

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
