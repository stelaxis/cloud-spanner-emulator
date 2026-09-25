# Keeping emulator state across restarts (`--data_dir`)

For development and tests only. By default the emulator keeps everything in
memory, exactly as upstream does. Started with `--data_dir=DIR`, it keeps its
state in `DIR` and gets it back when it restarts, after a clean shutdown or a
crash (`kill -9`, a killed container).

## Enabling it

| How you run it | Option |
|---|---|
| `emulator_main` | `--data_dir=DIR` |
| `gateway_main` (REST gateway, starts `emulator_main`) | `--data_dir=DIR`, or the environment variable `SPANNER_EMULATOR_DATA_DIR` |
| `build/macos/run_emulator.sh` | pass `--data_dir=DIR`; extra arguments go to `gateway_main` |
| Docker image | mount a volume at `/data` and set `SPANNER_EMULATOR_DATA_DIR=/data` |

```sh
docker run -p 9010:9010 -p 9020:9020 \
  -v spanner-data:/data -e SPANNER_EMULATOR_DATA_DIR=/data \
  ghcr.io/stelaxis/cloud-spanner-emulator:edge
```

The directory is created if it does not exist. Only one emulator can use a
directory at a time: a second one fails to start with `... LOCK is locked by
another emulator process`.

## What is kept

* Instances.
* GoogleSQL-dialect databases: their schema (tables, indexes, interleaving,
  foreign keys, check constraints, generated and default columns, commit
  timestamp columns, sequences, views, change stream definitions, proto
  bundles, database options set with `ALTER DATABASE`) and the latest version
  of every row.
* Sequence counters, as reservations: see [Guarantees](#guarantees).
* The clock: timestamps keep increasing across restarts.

## What is not kept

* **PostgreSQL-dialect databases.** Creating one with `--data_dir` fails with
  `FAILED_PRECONDITION`; run without `--data_dir` to use them.
* **Older row versions.** A read at a timestamp before the restart
  (`read_timestamp`, `exact_staleness`) fails with `FAILED_PRECONDITION`:
  "data versions from before a restart are not retained". Bounded-staleness
  reads pick a timestamp after the restart.
* Sessions and open transactions: clients see `Session not found` and retry,
  as after any emulator restart.
* Long-running operation history, backups, instance partitions, and change
  stream records.

## Guarantees

* **Acknowledged means durable.** A commit, schema change, or instance or
  database creation or drop is acknowledged only after its record is written
  and synced to the log. It survives a crash.
* **Unacknowledged may or may not survive.** If the emulator dies while a
  commit is in flight, the commit is either fully there after the restart or
  not at all, never partly. Clients that lost the reply cannot tell which,
  just as with Cloud Spanner.
* **Commit order.** The log holds every database's commits in one
  commit-timestamp order, and recovery replays them in that order.
* **Timestamps.** After a restart, every commit and read timestamp is later
  than every timestamp handed out before, even if the system clock went back.
  The emulator never hands out a timestamp past a synced lease, which it
  extends about 250 ms ahead at a time; it restarts above that lease, so its
  clock can run up to that much ahead of the system clock right after a
  restart, and the first reads wait that out. A read timestamp the client
  chooses (`read_timestamp`, `min_read_timestamp`), which `BeginTransaction`
  can return, is covered by the lease too: a read-only transaction at a
  future timestamp moves the lease there, and after a crash the clock resumes
  above it. So that the resumed clock stays within Spanner's range (up to
  9999-12-31T23:59:59.999999999Z), with `--data_dir` a chosen read timestamp
  later than 9999-12-31T22:59:59.749999999Z fails with `INVALID_ARGUMENT`,
  the error an out-of-range timestamp gets: that leaves room for the lease
  window (250 ms) and an hour of timestamps after a restart. Without
  `--data_dir` the full range is accepted.
* **Sequences** never hand out a value twice. The emulator reserves 1000
  counter values at a time and syncs the reservation before handing out any of
  them. After a crash, a sequence continues after its last reservation, so up
  to 1000 values are skipped.
* **Drops stay dropped.** A dropped database or instance does not come back,
  even if one with the same name is created later.
* **Durability depends on the file system's sync.** On Linux the log is
  synced with `fdatasync`. On macOS, `fsync` makes data survive a crash of the
  emulator or of the operating system, but not necessarily a power loss
  (see `F_FULLFSYNC` in `fcntl(2)`).

If a write or sync of the log fails, the operation fails with `UNAVAILABLE`,
and so does every later durable operation: the emulator must be restarted.
If the failed record belongs to a schema change, whose backfill writes are
already in memory (though not yet visible), or extends the clock lease, the
emulator exits instead. A schema change becomes visible, to `GetDatabaseDdl`
as to queries and transactions, only after its record is synced. A schema
change that is rejected as a whole (an invalid statement, or a database
option such as a retention period over 7 days) leaves nothing behind: its
backfill writes and dropped-table marks are removed from storage, with or
without `--data_dir`, and nothing is logged. Upstream kept them, which could
leave values of a rejected column type under the old schema.

## Files

```
DIR/LOCK                            held while an emulator uses DIR
DIR/checkpoint                      the state as of a log position
DIR/wal-00000000000000001234.log    log segments, named by their first record
```

Every file starts with a format version; an emulator refuses files of a
version it does not know rather than misread them. This one writes version 2,
which stores timestamps as seconds and nanoseconds so that any timestamp
Spanner accepts (up to the year 9999) is kept exactly. It still reads
version 1 directories, whose timestamps were int64 nanoseconds, and never
appends to a version 1 file: new records go to a new segment. Every record
carries a checksum and a sequence number. On startup:

* a damaged or incomplete *last* record is the remains of a write cut short
  by a crash: it is dropped and the file is truncated before anything is
  appended;
* any other damage, or a gap in the sequence numbers, stops the emulator
  with an error naming the file. Nothing is skipped or repaired.

A checkpoint is written in the background once the log has grown by 64 MiB
(`--data_dir_checkpoint_bytes`), or when the emulator receives `SIGUSR1`. It
is written to a temporary file, synced, renamed into place and the directory
synced; only then are the log segments it covers removed. Commits pause while
the checkpoint copies the data, not while it is written.

## Performance

Commits across all databases take turns to write and sync their log record,
so their throughput is bounded by the disk's sync latency. See the numbers in
[`verification/README.md`](../verification/README.md#persistence-results).
Without `--data_dir`, none of this code runs.

## Wiping the state

Stop the emulator and delete the directory (`rm -rf DIR`), or start it with a
new, empty directory. Do not edit or delete individual files: the log and the
checkpoint are only consistent together.

## How it works

[`verification/README.md`](../verification/README.md#persistence-implementation)
maps each rule of the Lean persistence model (`verification/lean/TxnSpec/
Persistence*.lean`) to the code, and lists where the implementation departs
from the model.
