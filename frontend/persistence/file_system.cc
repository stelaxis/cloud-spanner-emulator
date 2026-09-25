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

#include "frontend/persistence/file_system.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

namespace {

absl::Status ErrnoStatus(absl::string_view op, absl::string_view path,
                         int err) {
  std::string message = absl::StrCat(op, " ", path, ": ", std::strerror(err));
  if (err == ENOENT) return absl::NotFoundError(message);
  return absl::InternalError(message);
}

int SyncFd(int fd) {
#ifdef __APPLE__
  return fsync(fd);
#else
  return fdatasync(fd);
#endif
}

class PosixWritableFile : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd)
      : path_(std::move(path)), fd_(fd) {}
  ~PosixWritableFile() override { close(fd_); }

  absl::Status Append(absl::string_view data) override {
    while (!data.empty()) {
      ssize_t n = write(fd_, data.data(), data.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus("write", path_, errno);
      }
      data.remove_prefix(n);
    }
    return absl::OkStatus();
  }

  absl::Status Sync() override {
    if (SyncFd(fd_) != 0) return ErrnoStatus("fsync", path_, errno);
    return absl::OkStatus();
  }

 private:
  const std::string path_;
  const int fd_;
};

class PosixFileLock : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override {
    flock(fd_, LOCK_UN);
    close(fd_);
  }

 private:
  const int fd_;
};

class PosixFileSystemImpl : public FileSystem {
 public:
  absl::Status CreateDir(const std::string& dir) override {
    for (size_t i = 1; i <= dir.size(); ++i) {
      if (i < dir.size() && dir[i] != '/') continue;
      std::string prefix = dir.substr(0, i);
      if (mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
        return ErrnoStatus("mkdir", prefix, errno);
      }
    }
    return absl::OkStatus();
  }

  absl::StatusOr<std::vector<std::string>> ListDir(
      const std::string& dir) override {
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return ErrnoStatus("opendir", dir, errno);
    std::vector<std::string> names;
    while (struct dirent* entry = readdir(d)) {
      std::string name = entry->d_name;
      if (name != "." && name != "..") names.push_back(std::move(name));
    }
    closedir(d);
    return names;
  }

  absl::StatusOr<std::string> ReadFile(const std::string& path) override {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus("open", path, errno);
    std::string contents;
    char buffer[1 << 16];
    while (true) {
      ssize_t n = read(fd, buffer, sizeof(buffer));
      if (n < 0) {
        if (errno == EINTR) continue;
        int err = errno;
        close(fd);
        return ErrnoStatus("read", path, err);
      }
      if (n == 0) break;
      contents.append(buffer, n);
    }
    close(fd);
    return contents;
  }

  absl::StatusOr<std::unique_ptr<WritableFile>> OpenForAppend(
      const std::string& path, bool truncate) override {
    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    if (truncate) flags |= O_TRUNC;
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) return ErrnoStatus("open", path, errno);
    return std::make_unique<PosixWritableFile>(path, fd);
  }

  absl::Status Truncate(const std::string& path, uint64_t size) override {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus("open", path, errno);
    if (ftruncate(fd, static_cast<off_t>(size)) != 0 || SyncFd(fd) != 0) {
      int err = errno;
      close(fd);
      return ErrnoStatus("truncate", path, err);
    }
    close(fd);
    return absl::OkStatus();
  }

  absl::Status Rename(const std::string& from, const std::string& to) override {
    if (rename(from.c_str(), to.c_str()) != 0) {
      return ErrnoStatus("rename", from, errno);
    }
    return absl::OkStatus();
  }

  absl::Status Remove(const std::string& path) override {
    if (unlink(path.c_str()) != 0) return ErrnoStatus("unlink", path, errno);
    return absl::OkStatus();
  }

  absl::Status SyncDir(const std::string& dir) override {
    int fd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return ErrnoStatus("open", dir, errno);
    // Directory fsync is the documented way to make entries durable on Linux;
    // macOS accepts it as well.
    if (fsync(fd) != 0) {
      int err = errno;
      close(fd);
      return ErrnoStatus("fsync", dir, err);
    }
    close(fd);
    return absl::OkStatus();
  }

  absl::StatusOr<std::unique_ptr<FileLock>> Lock(
      const std::string& path) override {
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return ErrnoStatus("open", path, errno);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      int err = errno;
      close(fd);
      if (err == EWOULDBLOCK) {
        return absl::FailedPreconditionError(
            absl::StrCat(path, " is locked by another emulator process"));
      }
      return ErrnoStatus("flock", path, err);
    }
    return std::make_unique<PosixFileLock>(fd);
  }
};

}  // namespace

FileSystem* PosixFileSystem() {
  static FileSystem* fs = new PosixFileSystemImpl();
  return fs;
}

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
