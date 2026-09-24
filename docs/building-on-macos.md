# Building and running the emulator natively on macOS

The emulator builds and runs natively on Apple silicon (macOS arm64) with
Apple clang, without Docker. The Linux build is unaffected: every macOS
adjustment sits behind `build:macos` in `.bazelrc`, a
`select()` on `@platforms//os:macos`, or `#ifdef __APPLE__`.

## Prerequisites

- macOS 14 or later on Apple silicon.
- Xcode (not only the Command Line Tools), selected with
  `sudo xcode-select -s /Applications/Xcode.app`. Builds use Xcode's clang
  through `apple_support`'s toolchain.
- [Bazelisk](https://github.com/bazelbuild/bazelisk) as `bazel`. It picks up
  the Bazel version pinned in `.bazelversion`.
- A JDK, with `JAVA_HOME` pointing at it (the search-query parser is generated
  with JavaCC through `@local_jdk`), e.g. `brew install openjdk@21` and
  `export JAVA_HOME=$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home`.
- `pkg-config` on `PATH` (`brew install pkg-config`). ICU's build uses the
  system `make` and `pkg-config`.
- Optional: the Google Cloud CLI, to talk to the emulator and to run the
  gcloud tests (`GCLOUD_DIR=$(dirname $(which gcloud))`).

## Build

```bash
bazel build -c opt //binaries:emulator_main //binaries:gateway_main
```

`-c opt` matches the release image and is the only mode the binaries and
tests were verified in. In the default `fastbuild` mode, only ICU (the target
that needed `--strip=never`) was verified to build.

A cold build compiles about 13,000 actions. With Bazel limited to 6 CPUs on
an M-series machine that was also running other work, it took about 40
minutes. A no-op rebuild takes about 2 seconds. To limit Bazel's footprint on a
shared machine, add
`--jobs=6 --local_resources=cpu=6 --local_resources=memory=16384`.

## Run

```bash
build/macos/run_emulator.sh            # uses the binaries built above
build/macos/run_emulator.sh --build    # builds first
```

The script starts the REST gateway, which starts `emulator_main` (gRPC). The
defaults are ports 9010 (gRPC) and 9020 (REST) on `localhost`. Override them
with `SPANNER_EMULATOR_GRPC_PORT`, `SPANNER_EMULATOR_REST_PORT` and
`SPANNER_EMULATOR_HOST_NAME`. Extra arguments go to `gateway_main`, e.g.
`--log_requests`. To use a build configuration other than `-c opt`, set
`BAZEL_FLAGS`. It is split on whitespace and quotes are not interpreted, so
flag values containing spaces (such as a `--disk_cache` path with a space)
are not supported there. Stop the emulator with Ctrl-C or `kill` on the script.
Whenever the script exits, including when the gateway fails to start (for
example, because the REST port is taken), it stops `emulator_main` too. The
gateway itself never stops it.

Point clients at it as usual:

```bash
export SPANNER_EMULATOR_HOST=localhost:9010
gcloud config set api_endpoint_overrides/spanner http://localhost:9020/
```

## Test

```bash
GCLOUD_DIR=$(dirname $(which gcloud)) \
  bazel test -c opt --build_tests_only --keep_going //...
```

`--build_tests_only` skips PostgreSQL's standalone programs and loadable
modules, which do not link on macOS (see below). None of them is part of the
emulator.

Of the 237 test targets, 227 pass. The rest fail on macOS as follows:

| Target | Cause |
|---|---|
| `//third_party/spanner_pg/src/backend/nodes:serializer_deserializer_test`, `…/parser:parser_test`, `…/utils/cache:lsyscache_test` | Do not compile from this source tree on any platform (stray statement at file scope, a nonexistent `IndexElem::hash_partition`, an include of a nonexistent `third_party/googletest` path). |
| `//third_party/spanner_pg/datatypes/common/jsonb:jsonb_parse_test`, `…/datatypes/extended:pg_jsonb_conversion_functions_test`, `…/catalog:emulator_functions_test`, and `PGFunctionsTest.ToJsonB` in `//tests/conformance/endpoints:emulator_conformance_test` | PostgreSQL JSONB range, below. |
| `//tests/gcloud:instance_admin_test` | Instance and instance-partition timestamps have microsecond precision, below; the test expects nine fractional digits. |
| `//third_party/spanner_pg/src/backend/utils/adt:pg_locale_test` | macOS's `en_US` locale reports numeric grouping `"\3"`; glibc's reports the equivalent `"\3\3"`. |
| `//backend/schema/backfills:change_stream_backfill_test` | The partition-token generator reseeds `rand()` with the current microsecond on every call, so tokens generated within one microsecond can repeat. The test generates them in a tight loop, which a fast machine does within one microsecond. |

## What differs from the Linux build

- **Toolchain**: C/C++ builds with `apple_support`'s Xcode toolchain. Bazel's
  autoconfigured one rejects Xcode 16+'s implicit `SDKSettings.json` compile
  input. `.bazelrc` names the macOS-only repositories canonically, so
  `MODULE.bazel` gains no `bazel_dep`, and toolchain registration order stays
  the same on every platform.
- **ICU (`rules_foreign_cc`)**: uses the system `make` and `pkg-config` rather
  than building them from source, builds without stripping (`--strip=never`),
  and runs its in-action `make` at `-j4` instead of `-j32`. With stripping
  on, `apple_support` runs `strip` on every link, and it fails on configure's
  probe links, which have no `-o`.
- **Link flags**: no `-static-libgcc`/`-static-libstdc++` (Apple clang links
  the system libc++), no `-lrt` (part of libSystem), and no GNU-only
  `--no-fix-cortex-a53-843419`.
- **PostgreSQL headers**: `pg_config.h` reflects what macOS provides:
  `strlcpy`/`strlcat` in libc (so PostgreSQL's copies are not compiled),
  `<xlocale.h>`, `<sys/ucred.h>`, an `int`-returning `strerror_r`. Linux-only
  interfaces are marked unavailable (`epoll`, `posix_fadvise`,
  `posix_fallocate`, `sync_file_range`).
- **Generated node-tag list**: the `nodes.inc` genrule uses POSIX `sed`
  syntax on macOS. BSD `sed` silently produced an empty list, so PostgreSQL
  error messages named node types `<unknown:N>`.
- **Conformance tests**: `int64_t` is `long long` on macOS, so the test
  helper converts `long` arguments to `int64_t` before building a `Value`.
- **Deployment target**: binaries target macOS 14 (`--macos_minimum_os`), not
  the SDK version of the Xcode that built them.

## Known limitations

- **PostgreSQL JSONB numbers are limited to the range of `double`.**
  PostgreSQL-dialect JSONB numbers are parsed as `long double`, which allows
  up to 4,932 whole digits on Linux. On Apple silicon `long double` is the
  same as `double`, so JSONB numbers beyond about ±1.8e308 are rejected with
  `number overflow`.
- **Instance metadata timestamps have microsecond precision.** `createTime`
  and `updateTime` of instances and instance partitions come straight from the
  system clock, which ticks in microseconds on macOS, so they carry six
  fractional digits, not nine. Commit timestamps are unaffected: the emulator
  truncates them to microseconds on every platform.
- **PostgreSQL's standalone programs and loadable modules do not link**
  (`postgres`, `psql`, `libpq.so`, `dict_snowball.so`, `pgoutput.so` and the
  encoding conversion modules under `utils/mb/conversion_procs`). They use
  GNU-ld-only flags, and the emulator does not use them, so `bazel build //...`
  fails while `bazel test --build_tests_only //...` works.
- Only Apple silicon has been tried. Nothing in the changes is arm64-specific,
  but Intel Macs are untested.
- Bazel runs actions without a sandbox, so the bootstrap-catalog generator
  writes `pg_proc.dat` and `pg_aggregate.dat` into the source tree. They are
  git-ignored.
- `.bazelrc` refers to the macOS toolchain repositories by their Bazel 7
  canonical names (`apple_support~~…`, `rules_foreign_cc~`). Bazel 8 spells
  them with `+`, so a Bazel upgrade needs to update those lines.
