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

#include "frontend/persistence/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/synchronization/mutex.h"
#include "frontend/persistence/file_system.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {

namespace {

constexpr absl::string_view kSegmentMagic("CSEMWAL\0", 8);
constexpr absl::string_view kCheckpointMagic("CSEMCKP\0", 8);
constexpr size_t kFileHeaderSize = 16;  // magic, version, crc
constexpr uint32_t kRecordMagic = 0x4c575343;
constexpr size_t kRecordHeaderSize = 24;  // magic, length, lsn, crcs
constexpr absl::string_view kCheckpointName = "checkpoint";
constexpr absl::string_view kCheckpointTempName = "checkpoint.tmp";
constexpr absl::string_view kLockName = "LOCK";
constexpr absl::string_view kSegmentPrefix = "wal-";
constexpr absl::string_view kSegmentSuffix = ".log";

void PutFixed32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>(v >> (8 * i)));
}

void PutFixed64(std::string* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>(v >> (8 * i)));
}

uint32_t GetFixed32(absl::string_view data, size_t pos) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
  }
  return v;
}

uint64_t GetFixed64(absl::string_view data, size_t pos) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
  }
  return v;
}

uint32_t Crc(absl::string_view data) {
  return static_cast<uint32_t>(absl::ComputeCrc32c(data));
}

std::string FileHeader(absl::string_view magic) {
  std::string header(magic);
  PutFixed32(&header, kFormatVersion);
  PutFixed32(&header, Crc(header));
  return header;
}

absl::Status CheckFileHeader(absl::string_view data, absl::string_view magic,
                             const std::string& path) {
  if (data.size() < kFileHeaderSize || data.substr(0, magic.size()) != magic ||
      GetFixed32(data, 12) != Crc(data.substr(0, 12))) {
    return absl::DataLossError(
        absl::StrCat(path, " is not an emulator data file or is damaged"));
  }
  uint32_t version = GetFixed32(data, 8);
  if (version != kFormatVersion) {
    return absl::FailedPreconditionError(
        absl::StrCat(path, " has data format version ", version,
                     "; this emulator reads only version ", kFormatVersion,
                     ". Use a matching emulator, or wipe the data directory."));
  }
  return absl::OkStatus();
}

enum class Frame { kOk, kIncomplete, kBad };

Frame ParseFrame(absl::string_view data, size_t pos, uint64_t* lsn,
                 absl::string_view* payload) {
  if (data.size() - pos < kRecordHeaderSize) return Frame::kIncomplete;
  absl::string_view header = data.substr(pos, kRecordHeaderSize);
  if (GetFixed32(header, 0) != kRecordMagic ||
      GetFixed32(header, 20) != Crc(header.substr(0, 20))) {
    return Frame::kBad;
  }
  uint64_t length = GetFixed32(header, 4);
  if (data.size() - pos - kRecordHeaderSize < length) {
    return Frame::kIncomplete;
  }
  absl::string_view body = data.substr(pos + kRecordHeaderSize, length);
  if (GetFixed32(header, 16) != Crc(body)) return Frame::kBad;
  *lsn = GetFixed64(header, 8);
  *payload = body;
  return Frame::kOk;
}

// Whether a complete, intact record starts anywhere at or after `pos`.
bool IntactRecordFrom(absl::string_view data, size_t pos) {
  for (; pos + kRecordHeaderSize <= data.size(); ++pos) {
    uint64_t lsn;
    absl::string_view payload;
    if (GetFixed32(data, pos) == kRecordMagic &&
        ParseFrame(data, pos, &lsn, &payload) == Frame::kOk) {
      return true;
    }
  }
  return false;
}

std::string SegmentName(uint64_t first_lsn) {
  return absl::StrFormat("%s%020d%s", kSegmentPrefix, first_lsn,
                         kSegmentSuffix);
}

bool ParseSegmentName(absl::string_view name, uint64_t* first_lsn) {
  if (!absl::ConsumePrefix(&name, kSegmentPrefix) ||
      !absl::ConsumeSuffix(&name, kSegmentSuffix) || name.size() != 20) {
    return false;
  }
  return absl::SimpleAtoi(name, first_lsn);
}

struct Segment {
  uint64_t first_lsn;
  std::string path;
  std::string data;
  std::vector<absl::string_view> records;
  size_t valid_end = kFileHeaderSize;  // after the last intact record
  bool header_torn = false;
};

// Parses the records of `segment`. A damaged record is a torn write only at
// the end of the last segment, with no intact record after it.
absl::Status ScanSegment(Segment& segment, bool is_last) {
  absl::string_view data = segment.data;
  if (data.size() < kFileHeaderSize && is_last) {
    segment.header_torn = true;
    return absl::OkStatus();
  }
  if (auto status = CheckFileHeader(data, kSegmentMagic, segment.path);
      !status.ok()) {
    return status;
  }
  size_t pos = kFileHeaderSize;
  while (pos < data.size()) {
    uint64_t lsn;
    absl::string_view payload;
    Frame frame = ParseFrame(data, pos, &lsn, &payload);
    if (frame == Frame::kOk) {
      uint64_t expected = segment.first_lsn + segment.records.size();
      if (lsn != expected) {
        return absl::DataLossError(
            absl::StrCat(segment.path, ": record at offset ", pos, " has LSN ",
                         lsn, ", expected ", expected));
      }
      segment.records.push_back(payload);
      pos += kRecordHeaderSize + payload.size();
      segment.valid_end = pos;
      continue;
    }
    if (IntactRecordFrom(data, pos + 1)) {
      return absl::DataLossError(
          absl::StrCat(segment.path, ": damaged record at offset ", pos,
                       " is followed by intact records; refusing to skip it"));
    }
    if (!is_last) {
      return absl::DataLossError(
          absl::StrCat(segment.path, ": damaged record at offset ", pos,
                       " in a segment that is not the last"));
    }
    break;  // A torn final record.
  }
  return absl::OkStatus();
}

}  // namespace

std::string FrameRecord(uint64_t lsn, absl::string_view payload) {
  std::string frame;
  frame.reserve(kRecordHeaderSize + payload.size());
  PutFixed32(&frame, kRecordMagic);
  PutFixed32(&frame, static_cast<uint32_t>(payload.size()));
  PutFixed64(&frame, lsn);
  PutFixed32(&frame, Crc(payload));
  PutFixed32(&frame, Crc(frame));
  frame.append(payload);
  return frame;
}

std::string Log::Path(absl::string_view name) const {
  return absl::StrCat(dir_, "/", name);
}

absl::StatusOr<std::unique_ptr<Log>> Log::Open(FileSystem* fs,
                                               const std::string& dir,
                                               LogContents* contents) {
  auto log = absl::WrapUnique(new Log(fs, dir));
  absl::MutexLock lock(log->mu_);
  *contents = LogContents();
  if (auto status = fs->CreateDir(dir); !status.ok()) return status;
  auto file_lock = fs->Lock(log->Path(kLockName));
  if (!file_lock.ok()) return file_lock.status();
  log->lock_ = *std::move(file_lock);

  auto names = fs->ListDir(dir);
  if (!names.ok()) return names.status();
  std::vector<Segment> segments;
  for (const std::string& name : *names) {
    uint64_t first_lsn;
    if (name == kCheckpointTempName) {
      // An unpublished checkpoint; it never counts.
      if (auto status = fs->Remove(log->Path(name)); !status.ok()) {
        return status;
      }
    } else if (ParseSegmentName(name, &first_lsn)) {
      segments.push_back(Segment{first_lsn, log->Path(name)});
    }
  }
  std::sort(segments.begin(), segments.end(),
            [](const Segment& a, const Segment& b) {
              return a.first_lsn < b.first_lsn;
            });

  auto checkpoint = fs->ReadFile(log->Path(kCheckpointName));
  if (checkpoint.ok()) {
    const std::string path = log->Path(kCheckpointName);
    if (auto status = CheckFileHeader(*checkpoint, kCheckpointMagic, path);
        !status.ok()) {
      return status;
    }
    uint64_t boundary;
    absl::string_view payload;
    if (ParseFrame(*checkpoint, kFileHeaderSize, &boundary, &payload) !=
            Frame::kOk ||
        kFileHeaderSize + kRecordHeaderSize + payload.size() !=
            checkpoint->size()) {
      return absl::DataLossError(absl::StrCat(path, " is damaged"));
    }
    contents->checkpoint = std::string(payload);
    contents->boundary = boundary;
  } else if (!absl::IsNotFound(checkpoint.status())) {
    return checkpoint.status();
  }
  log->checkpoint_boundary_ = contents->boundary;

  for (Segment& segment : segments) {
    auto data = fs->ReadFile(segment.path);
    if (!data.ok()) return data.status();
    segment.data = *std::move(data);
  }
  // A last segment holding no more than a header may be left over from a
  // failed StartSegment, in which case the one before it is really the last.
  const size_t n = segments.size();
  const bool maybe_leftover =
      n > 1 && segments.back().data.size() <= kFileHeaderSize;
  for (size_t i = 0; i < n; ++i) {
    bool is_last = i + 1 == n || (maybe_leftover && i + 2 == n);
    if (auto status = ScanSegment(segments[i], is_last); !status.ok()) {
      return status;
    }
  }
  if (maybe_leftover) {
    const Segment& prev = segments[n - 2];
    if (segments.back().first_lsn != prev.first_lsn + prev.records.size()) {
      if (auto status = fs->Remove(segments.back().path); !status.ok()) {
        return status;
      }
      segments.pop_back();
    } else if (prev.valid_end < prev.data.size()) {
      return absl::DataLossError(
          absl::StrCat(prev.path, ": damaged record at offset ", prev.valid_end,
                       " in a segment that is not the last"));
    }
  }

  uint64_t expected = contents->boundary;
  for (size_t i = 0; i < segments.size(); ++i) {
    const Segment& segment = segments[i];
    if (i > 0) {
      const Segment& prev = segments[i - 1];
      if (segment.first_lsn != prev.first_lsn + prev.records.size()) {
        return absl::DataLossError(
            absl::StrCat(segment.path, " starts at LSN ", segment.first_lsn,
                         " but the previous segment ends at LSN ",
                         prev.first_lsn + prev.records.size()));
      }
    }
    for (size_t j = 0; j < segment.records.size(); ++j) {
      uint64_t lsn = segment.first_lsn + j;
      if (lsn < contents->boundary) continue;
      if (lsn != expected) {
        return absl::DataLossError(
            absl::StrCat("records ", expected, " to ", lsn - 1,
                         " after the checkpoint are missing"));
      }
      contents->records.emplace_back(segment.records[j]);
      ++expected;
    }
  }

  uint64_t next_lsn = contents->boundary;
  if (!segments.empty()) {
    Segment& last = segments.back();
    next_lsn = std::max(next_lsn, last.first_lsn + last.records.size());
    if (last.header_torn) {
      contents->discarded_tail_bytes = last.data.size();
      if (auto status = fs->Remove(last.path); !status.ok()) return status;
      segments.pop_back();
    } else if (last.valid_end < last.data.size()) {
      // Truncate the torn bytes before anything is appended after them.
      contents->discarded_tail_bytes = last.data.size() - last.valid_end;
      if (auto status = fs->Truncate(last.path, last.valid_end); !status.ok()) {
        return status;
      }
    }
  }
  for (const Segment& segment : segments) {
    log->segments_.push_back(segment.first_lsn);
  }
  log->next_lsn_ = next_lsn;
  if (segments.empty() ||
      segments.back().first_lsn + segments.back().records.size() != next_lsn) {
    if (auto status = log->CreateSegment(next_lsn); !status.ok()) {
      return status;
    }
  } else {
    auto file = fs->OpenForAppend(segments.back().path, /*truncate=*/false);
    if (!file.ok()) return file.status();
    log->current_ = *std::move(file);
    log->current_bytes_ = segments.back().valid_end - kFileHeaderSize;
  }
  return log;
}

absl::Status Log::CreateSegment(uint64_t first_lsn) {
  const std::string path = Path(SegmentName(first_lsn));
  absl::Status status;
  auto file = fs_->OpenForAppend(path, /*truncate=*/true);
  if (file.ok()) {
    status = (*file)->Append(FileHeader(kSegmentMagic));
    if (status.ok()) status = (*file)->Sync();
    if (status.ok()) status = fs_->SyncDir(dir_);
  } else {
    status = file.status();
  }
  if (!status.ok()) {
    // Keep appending to the current segment. A leftover empty segment would
    // sit between it and the next one, so failing to remove it breaks the log.
    auto removed = fs_->Remove(path);
    if (!removed.ok() && !absl::IsNotFound(removed)) broken_ = status;
    return status;
  }
  current_ = *std::move(file);
  current_bytes_ = 0;
  segments_.push_back(first_lsn);
  return absl::OkStatus();
}

absl::StatusOr<uint64_t> Log::Append(absl::string_view payload) {
  absl::MutexLock lock(mu_);
  if (!broken_.ok()) return broken_;
  uint64_t lsn = next_lsn_;
  std::string frame = FrameRecord(lsn, payload);
  absl::Status status = current_->Append(frame);
  if (status.ok()) status = current_->Sync();
  if (!status.ok()) {
    // The record may or may not have reached the disk, and later records
    // must not follow torn bytes, so no further record is accepted.
    broken_ = absl::UnavailableError(absl::StrCat(
        "Cannot write the emulator data directory: ", status.message(),
        ". Restart the emulator."));
    return broken_;
  }
  ++next_lsn_;
  current_bytes_ += frame.size();
  return lsn;
}

absl::StatusOr<uint64_t> Log::StartSegment() {
  absl::MutexLock lock(mu_);
  if (!broken_.ok()) return broken_;
  if (current_bytes_ == 0) return next_lsn_;
  if (auto status = CreateSegment(next_lsn_); !status.ok()) return status;
  return next_lsn_;
}

absl::Status Log::PublishCheckpoint(uint64_t boundary,
                                    absl::string_view payload) {
  const std::string temp = Path(kCheckpointTempName);
  auto file = fs_->OpenForAppend(temp, /*truncate=*/true);
  if (!file.ok()) return file.status();
  std::string contents = FileHeader(kCheckpointMagic);
  contents.append(FrameRecord(boundary, payload));
  if (auto status = (*file)->Append(contents); !status.ok()) return status;
  if (auto status = (*file)->Sync(); !status.ok()) return status;
  if (auto status = fs_->Rename(temp, Path(kCheckpointName)); !status.ok()) {
    return status;
  }
  if (auto status = fs_->SyncDir(dir_); !status.ok()) return status;
  absl::MutexLock lock(mu_);
  checkpoint_boundary_ = std::max(checkpoint_boundary_, boundary);
  return absl::OkStatus();
}

absl::Status Log::RemoveSegmentsBelow(uint64_t boundary) {
  absl::MutexLock lock(mu_);
  boundary = std::min(boundary, checkpoint_boundary_);
  size_t removed = 0;
  absl::Status status;
  while (removed + 1 < segments_.size() && segments_[removed + 1] <= boundary) {
    status = fs_->Remove(Path(SegmentName(segments_[removed])));
    if (!status.ok() && !absl::IsNotFound(status)) break;
    status = absl::OkStatus();
    ++removed;
  }
  segments_.erase(segments_.begin(), segments_.begin() + removed);
  if (removed > 0) {
    absl::Status synced = fs_->SyncDir(dir_);
    if (status.ok()) status = synced;
  }
  return status;
}

uint64_t Log::CurrentSegmentBytes() {
  absl::MutexLock lock(mu_);
  return current_bytes_;
}

absl::Status Log::health() {
  absl::MutexLock lock(mu_);
  return broken_;
}

}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
