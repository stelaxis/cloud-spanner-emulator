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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/crc/crc32c.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "frontend/persistence/file_system.h"
#include "frontend/persistence/mem_file_system.h"
#include "gmock/gmock.h"
#include "googlesql/base/testing/status_matchers.h"
#include "gtest/gtest.h"

namespace google {
namespace spanner {
namespace emulator {
namespace frontend {
namespace persistence {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using Op = MemFileSystem::Op;

constexpr char kDir[] = "/data";
constexpr size_t kFileHeaderSize = 16;
constexpr size_t kRecordHeaderSize = 24;

std::string SegmentPath(uint64_t first_lsn) {
  return absl::StrCat(kDir, "/wal-",
                      std::string(20 - absl::StrCat(first_lsn).size(), '0'),
                      first_lsn, ".log");
}

class LogTest : public ::testing::Test {
 protected:
  std::unique_ptr<Log> Open(LogContents* contents) {
    auto log = Log::Open(fs_.get(), kDir, contents);
    EXPECT_TRUE(log.ok()) << log.status();
    return log.ok() ? *std::move(log) : nullptr;
  }

  absl::Status OpenStatus() {
    LogContents contents;
    return Log::Open(fs_.get(), kDir, &contents).status();
  }

  // Simulates kill -9 plus losing everything not synced: the log object is
  // dropped (releasing its lock) and unsynced bytes are lost, except for the
  // first `keep` bytes of each unsynced tail.
  void Crash(std::unique_ptr<Log>& log, uint64_t keep = 0) {
    log.reset();
    fs_->SetHook(nullptr);
    fs_->Crash(keep);
  }

  void FailOn(Op failing, int skip = 0) {
    auto count = std::make_shared<int>(0);
    fs_->SetHook([failing, skip, count](Op op, const std::string&) {
      if (op == failing && (*count)++ >= skip) {
        return absl::InternalError("injected I/O error");
      }
      return absl::OkStatus();
    });
  }

  std::unique_ptr<MemFileSystem> fs_ = std::make_unique<MemFileSystem>();
};

TEST_F(LogTest, RecordsSurviveACrash) {
  LogContents contents;
  auto log = Open(&contents);
  EXPECT_THAT(log->Append("a"), googlesql_base::testing::IsOkAndHolds(0));
  EXPECT_THAT(log->Append("b"), googlesql_base::testing::IsOkAndHolds(1));
  Crash(log);
  log = Open(&contents);
  EXPECT_FALSE(contents.checkpoint.has_value());
  EXPECT_THAT(contents.records, ElementsAre("a", "b"));
  EXPECT_THAT(log->Append("c"), googlesql_base::testing::IsOkAndHolds(2));
  Crash(log);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b", "c"));
}

TEST_F(LogTest, TornTailIsDiscardedAndTruncatedBeforeTheNextAppend) {
  const std::string payload(100, 'x');
  const size_t frame = kRecordHeaderSize + payload.size();
  for (size_t keep = 1; keep < frame; ++keep) {
    SCOPED_TRACE(keep);
    fs_ = std::make_unique<MemFileSystem>();
    LogContents contents;
    auto log = Open(&contents);
    GOOGLESQL_ASSERT_OK(log->Append("first").status());
    // The append's bytes are written but its sync never happens.
    FailOn(Op::kSync);
    EXPECT_FALSE(log->Append(payload).ok());
    Crash(log, keep);
    log = Open(&contents);
    EXPECT_THAT(contents.records, ElementsAre("first"));
    EXPECT_EQ(contents.discarded_tail_bytes, keep);
    GOOGLESQL_ASSERT_OK(log->Append("second").status());
    Crash(log);
    Open(&contents);
    EXPECT_THAT(contents.records, ElementsAre("first", "second"));
  }
}

TEST_F(LogTest, FailedSyncIsNotAcknowledgedAndBreaksTheLog) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("durable").status());
  FailOn(Op::kSync);
  EXPECT_THAT(
      log->Append("lost").status(),
      googlesql_base::testing::StatusIs(absl::StatusCode::kUnavailable));
  fs_->SetHook(nullptr);
  // Later records must not follow bytes that may be torn.
  EXPECT_FALSE(log->Append("after").ok());
  EXPECT_FALSE(log->health().ok());
  Crash(log);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("durable"));
}

TEST_F(LogTest, FailedWriteIsNotAcknowledged) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("durable").status());
  FailOn(Op::kAppend);
  EXPECT_FALSE(log->Append("lost").ok());
  Crash(log);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("durable"));
}

TEST_F(LogTest, UnacknowledgedRecordThatReachedTheDiskMayReappear) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("durable").status());
  FailOn(Op::kSync);
  EXPECT_FALSE(log->Append("unacknowledged").ok());
  // The whole record reached the disk before the crash.
  Crash(log, /*keep=*/1 << 20);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("durable", "unacknowledged"));
}

TEST_F(LogTest, CorruptRecordMidLogFailsLoud) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("first record").status());
  GOOGLESQL_ASSERT_OK(log->Append("second record").status());
  GOOGLESQL_ASSERT_OK(log->Append("third record").status());
  Crash(log);
  const std::string path = SegmentPath(0);
  std::string data = *fs_->Contents(path);
  const size_t second = kFileHeaderSize + kRecordHeaderSize + 12;
  for (size_t offset : {second + 2, second + kRecordHeaderSize + 3}) {
    SCOPED_TRACE(offset);
    std::string corrupt = data;
    corrupt[offset] ^= 0x40;
    fs_->Overwrite(path, corrupt);
    absl::Status status = OpenStatus();
    EXPECT_THAT(status,
                googlesql_base::testing::StatusIs(absl::StatusCode::kDataLoss));
    EXPECT_THAT(status.message(), HasSubstr("followed by intact records"));
  }
  // The damage is not repaired or skipped: the file is unchanged.
  EXPECT_NE(*fs_->Contents(path), data);
}

TEST_F(LogTest, DamageInAnEarlierSegmentFailsLoud) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  GOOGLESQL_ASSERT_OK(log->StartSegment().status());
  GOOGLESQL_ASSERT_OK(log->Append("c").status());
  Crash(log);
  std::string data = *fs_->Contents(SegmentPath(0));
  data.back() ^= 1;  // the last record of a segment that is not the last
  fs_->Overwrite(SegmentPath(0), data);
  EXPECT_THAT(OpenStatus(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kDataLoss));
}

TEST_F(LogTest, MissingSegmentFailsLoud) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  GOOGLESQL_ASSERT_OK(log->StartSegment().status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  GOOGLESQL_ASSERT_OK(log->StartSegment().status());
  GOOGLESQL_ASSERT_OK(log->Append("c").status());
  Crash(log);
  GOOGLESQL_ASSERT_OK(fs_->Remove(SegmentPath(1)));
  GOOGLESQL_ASSERT_OK(fs_->SyncDir(kDir));
  EXPECT_THAT(OpenStatus(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kDataLoss));
}

TEST_F(LogTest, DamagedFileHeaderFailsLoud) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  Crash(log);
  std::string data = *fs_->Contents(SegmentPath(0));
  data[3] ^= 1;
  fs_->Overwrite(SegmentPath(0), data);
  EXPECT_THAT(OpenStatus(),
              googlesql_base::testing::StatusIs(absl::StatusCode::kDataLoss));
}

// Rewrites the format version in the header of `path`, with a valid checksum.
void SetFormatVersion(MemFileSystem& fs, const std::string& path,
                      uint8_t version) {
  std::string data = *fs.Contents(path);
  data[8] = static_cast<char>(version);
  uint32_t crc = static_cast<uint32_t>(
      absl::ComputeCrc32c(absl::string_view(data).substr(0, 12)));
  for (int i = 0; i < 4; ++i) data[12 + i] = static_cast<char>(crc >> (8 * i));
  fs.Overwrite(path, data);
}

TEST_F(LogTest, UnknownFormatVersionFailsLoud) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  Crash(log);
  for (uint8_t version : {0, 3}) {
    SetFormatVersion(*fs_, SegmentPath(0), version);
    absl::Status status = OpenStatus();
    EXPECT_THAT(status, googlesql_base::testing::StatusIs(
                            absl::StatusCode::kFailedPrecondition));
    EXPECT_THAT(status.message(),
                HasSubstr(absl::StrCat("format version ", version)));
  }
}

TEST_F(LogTest, OlderFormatIsReadButNotAppendedTo) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  Crash(log);
  SetFormatVersion(*fs_, SegmentPath(0), 1);
  const std::string version1 = *fs_->Contents(SegmentPath(0));
  log = Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b"));
  GOOGLESQL_ASSERT_OK(log->Append("c").status());
  Crash(log);
  // "c" went to a new segment of the current format; the old file is as it
  // was.
  EXPECT_EQ(*fs_->Contents(SegmentPath(0)), version1);
  EXPECT_EQ((*fs_->Contents(SegmentPath(2)))[8], kFormatVersion);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b", "c"));
}

TEST_F(LogTest, SecondOpenOfTheSameDirectoryIsRefused) {
  LogContents contents;
  auto log = Open(&contents);
  EXPECT_THAT(OpenStatus(), googlesql_base::testing::StatusIs(
                                absl::StatusCode::kFailedPrecondition));
  log.reset();
  GOOGLESQL_EXPECT_OK(OpenStatus());
}

TEST_F(LogTest, CheckpointSkipsCoveredRecords) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  auto boundary = log->StartSegment();
  GOOGLESQL_ASSERT_OK(boundary.status());
  EXPECT_EQ(*boundary, 2);
  GOOGLESQL_ASSERT_OK(
      log->Append("c").status());  // concurrent with the checkpoint
  GOOGLESQL_ASSERT_OK(log->PublishCheckpoint(*boundary, "image of a, b"));
  GOOGLESQL_ASSERT_OK(log->Append("d").status());
  Crash(log);
  log = Open(&contents);
  EXPECT_EQ(contents.checkpoint, "image of a, b");
  EXPECT_EQ(contents.boundary, 2);
  EXPECT_THAT(contents.records, ElementsAre("c", "d"));
}

TEST_F(LogTest, CrashBetweenCheckpointWriteAndRename) {
  for (Op failing : {Op::kSync, Op::kRename}) {
    fs_ = std::make_unique<MemFileSystem>();
    LogContents contents;
    auto log = Open(&contents);
    GOOGLESQL_ASSERT_OK(log->Append("a").status());
    auto boundary = log->StartSegment();
    GOOGLESQL_ASSERT_OK(boundary.status());
    GOOGLESQL_ASSERT_OK(log->Append("b").status());
    FailOn(failing);
    EXPECT_FALSE(log->PublishCheckpoint(*boundary, "image").ok());
    Crash(log, /*keep=*/1 << 20);
    log = Open(&contents);
    EXPECT_FALSE(contents.checkpoint.has_value());
    EXPECT_THAT(contents.records, ElementsAre("a", "b"));
    // The unpublished temporary file is gone.
    EXPECT_FALSE(fs_->Contents(absl::StrCat(kDir, "/checkpoint.tmp")).ok());
  }
}

TEST_F(LogTest, RenameWithoutDirectorySyncIsNotPublished) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  auto boundary = log->StartSegment();
  GOOGLESQL_ASSERT_OK(boundary.status());
  FailOn(Op::kSyncDir);
  EXPECT_FALSE(log->PublishCheckpoint(*boundary, "image").ok());
  Crash(log);
  Open(&contents);
  EXPECT_FALSE(contents.checkpoint.has_value());
  EXPECT_THAT(contents.records, ElementsAre("a"));
}

TEST_F(LogTest, CrashBetweenCheckpointAndTruncation) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  auto boundary = log->StartSegment();
  GOOGLESQL_ASSERT_OK(boundary.status());
  GOOGLESQL_ASSERT_OK(log->Append("c").status());
  GOOGLESQL_ASSERT_OK(log->PublishCheckpoint(*boundary, "image"));
  Crash(log);  // before RemoveSegmentsBelow
  GOOGLESQL_EXPECT_OK(fs_->Contents(SegmentPath(0)).status());
  log = Open(&contents);
  EXPECT_EQ(contents.checkpoint, "image");
  EXPECT_THAT(contents.records, ElementsAre("c"));

  GOOGLESQL_ASSERT_OK(log->RemoveSegmentsBelow(contents.boundary));
  EXPECT_FALSE(fs_->Contents(SegmentPath(0)).ok());
  GOOGLESQL_ASSERT_OK(log->Append("d").status());
  Crash(log);
  Open(&contents);
  EXPECT_EQ(contents.checkpoint, "image");
  EXPECT_THAT(contents.records, ElementsAre("c", "d"));
}

TEST_F(LogTest, TruncationNeverPassesThePublishedBoundary) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  auto boundary = log->StartSegment();
  GOOGLESQL_ASSERT_OK(boundary.status());
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  // Not published yet: nothing may be removed.
  GOOGLESQL_ASSERT_OK(log->RemoveSegmentsBelow(*boundary));
  GOOGLESQL_EXPECT_OK(fs_->Contents(SegmentPath(0)).status());
  Crash(log);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b"));
}

TEST_F(LogTest, FailedSegmentStartKeepsTheLogUsable) {
  LogContents contents;
  auto log = Open(&contents);
  GOOGLESQL_ASSERT_OK(log->Append("a").status());
  FailOn(Op::kSync);
  EXPECT_FALSE(log->StartSegment().ok());
  fs_->SetHook(nullptr);
  GOOGLESQL_ASSERT_OK(log->Append("b").status());
  Crash(log, /*keep=*/1 << 20);
  log = Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b"));
  GOOGLESQL_ASSERT_OK(log->Append("c").status());
  Crash(log);
  Open(&contents);
  EXPECT_THAT(contents.records, ElementsAre("a", "b", "c"));
}

TEST_F(LogTest, PosixFileSystemRoundTrip) {
  std::string dir = absl::StrCat(::testing::TempDir(), "/log_test_",
                                 absl::ToUnixNanos(absl::Now()));
  FileSystem* fs = PosixFileSystem();
  LogContents contents;
  auto log = Log::Open(fs, dir, &contents);
  GOOGLESQL_ASSERT_OK(log.status());
  GOOGLESQL_ASSERT_OK((*log)->Append("a").status());
  LogContents second;
  EXPECT_THAT(
      Log::Open(fs, dir, &second).status(),
      googlesql_base::testing::StatusIs(absl::StatusCode::kFailedPrecondition));
  log->reset();
  auto reopened = Log::Open(fs, dir, &contents);
  GOOGLESQL_ASSERT_OK(reopened.status());
  EXPECT_THAT(contents.records, ElementsAre("a"));
}

}  // namespace
}  // namespace persistence
}  // namespace frontend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
