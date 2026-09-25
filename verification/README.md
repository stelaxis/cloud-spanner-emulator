# Transaction model: proofs and conformance

Before changing how the emulator runs read-write transactions, this directory
pins down two things:

* **Upstream**: what the emulator does today (v1.5.58, one lock slot per
  database). A Lean 4 model, checked step by step against the real emulator.
* **Target**: the optimistic design meant to replace it. A Lean 4 model with
  proofs that it is serializable, that transactions touching disjoint data
  don't abort each other, and that it accepts every history Upstream commits.

The conformance harness runs the same random schedules through the emulator
and a model and diffs every step. The fork's emulator implements **Target**
(see [the implementation](#the-implementation)) and must match it; upstream
1.5.58 matches **Upstream**.

```
verification/
  lean/                    Lake project, Lean v4.34.0, no Mathlib
    TxnSpec/Basic.lean       keys, key sets, views, operations, results
    TxnSpec/Local.lean       one transaction's reads/writes over a base view; footprints
    TxnSpec/Mvcc.lean        the MVCC store (commit log), transaction state
    TxnSpec/Upstream.lean    Upstream model
    TxnSpec/Target.lean      Target model
    TxnSpec/Frame.lean       an operation depends on the base view only through its footprint
    TxnSpec/Serial.lean      serializability, serial execution, timestamp lemmas
    TxnSpec/TargetProofs.lean, UpstreamProofs.lean, Refinement.lean, Parallel.lean
    TxnSpec/Counterexamples.lean   design variants that break a property, checked by `decide`
    TxnSpec/Json.lean, TxnModel.lean   the `txnmodel` executable
  conformance/             Go harness (raw gRPC), one driver goroutine
  stress/                  Go-client bank-transfer stress test, 16 workers
```

## What is modelled

Two tables, `A` and `B`, each `(K INT64 NOT NULL, V INT64) PRIMARY KEY (K)`.
Keys are 0–15. A row either exists with a value or is absent.

Operations, per transaction:

| op | emulator call |
|---|---|
| `begin_rw` | `BeginTransaction(read_write)` |
| `begin_ro` (strong, or at the `n`-th commit's timestamp) | `BeginTransaction(read_only{strong \| read_timestamp})` |
| `read` table, key set | `Read` with one key, one range (open/closed ends), or `all` |
| `sql` table, key set | `ExecuteSql("SELECT K, V FROM t WHERE <pred> ORDER BY K")` |
| `dml_insert` / `dml_update` / `dml_delete` | `ExecuteSql` DML with a `K` predicate |
| `commit` mutations | `Commit` with `insert` / `update` / `insert_or_update` / `delete` mutations |
| `rollback` | `Rollback` |

Results: rows, a DML row count, `committed` with the commit's index, `ok`, or
an error code: `ABORTED`, `ALREADY_EXISTS`, `NOT_FOUND`, `INVALID_ARGUMENT`,
`FAILED_PRECONDITION`.

Within a transaction, both models share the same semantics (`Local.lean`).
Reads see the transaction's own buffered writes. `insert` of a visible row
fails with `ALREADY_EXISTS`. `update` of an invisible row fails with
`NOT_FOUND`, or with `INVALID_ARGUMENT` if this transaction deleted the row
(`read_write_transaction.cc:571-576`). Only `insert` and `insert_or_update`
clear a row from the deleted list (`:550-567`). Mutations in one `Commit`
apply in order.

The store is the commit log. The `i`-th commit has timestamp `i+1`, and
`stateAt log n` is the database as of timestamp `n`: exactly the commits with
`ts ≤ n`. That is the emulator's key → column → time → value store, with
tombstones.

### Upstream (`Upstream.lean`), abort probability 0

* One lock slot per database. Every RW read or buffered write enqueues a lock
  request (`transaction_store.cc:97,120,149,213,245,390`). If the slot is free
  or already this transaction's, the request is granted. Otherwise the
  requester is aborted immediately; nothing waits (`locking/manager.cc:55-84`).
  The wound branch (`:69-79`) is disabled at probability 0.
* A `Commit` with mutations takes the slot through the same path. A `Commit`
  with none takes it in `ReserveCommitTimestamp`, or aborts if another
  transaction holds it (`manager.cc:100-117`).
* The slot holder reads the latest committed state (`InfiniteFuture`,
  `transaction_store.cc:215,284`). Commit runs reserve → flush →
  `MarkCommitted` → `UnlockAll` (`read_write_transaction.cc:627-641`).
* Any error resets the backend transaction and releases the slot
  (`read_write_transaction.cc:378-393`). The frontend then returns the same
  error for every later call except `Rollback`
  (`frontend/entities/transaction.cc:405,413-425`). `BeginTransaction` takes no
  lock (`handlers/transactions.cc:58`).
* RO transactions take no locks. They read at their timestamp: `Now()` for a
  strong read (`read_only_transaction.cc:117`). The clock hands out strictly
  increasing timestamps (`common/clock.cc:46`).

### Target (`Target.lean`)

* A RW transaction takes its snapshot at its first data operation, or at
  `Commit` if it has none. It reads that snapshot plus its own buffer. Each
  operation adds its footprint (`fp`) to the read set:
  * `read`: the key set.
  * `sql`, `dml_update`, `dml_delete`: the whole table. With `--pushdown`,
    only the predicate's key set.
  * `dml_insert` and each mutation: its row. The emulator looks up each
    mutation's row while flattening it (`FlattenNonDeleteOpRow`,
    `FlattenDeleteOp`).
* `Commit` runs atomically, under the per-database commit mutex:
  1. If a commit after the snapshot wrote a row in the read set, including
     the mutations' rows → `ABORTED`.
  2. Otherwise apply the mutations, take timestamp `len + 1`, and append.
* A constraint error raised against a stale snapshot is reported as `ABORTED`
  (`validateErrors`; see the second flaw below).
* RO transactions work as in Upstream.

`Cfg` flags turn on design variants so `Counterexamples.lean` can show what
breaks: `snapAtBegin`, `validateErrors := false`, `keysOnlyReadSet`.

## What is proved

`lake build` checks everything. There is no `sorry`. The theorems use only
Lean core's `propext`, `Classical.choice` and `Quot.sound`: no `native_decide`,
no custom axioms (`#print axioms`). Unless stated otherwise, Target theorems
hold for every `Cfg` with `keysOnlyReadSet = false`, including both
`pushdown` settings.

| Theorem | Statement |
|---|---|
| `Target.target_serializable` | **(i)** For every schedule, each committed transaction's results (rows, counts) and writes equal those of running the committed programs one at a time, in commit-timestamp order, from the empty database. The final store equals that serial result. This rules out lost updates, write skew and phantoms. |
| `Target.target_ts_strict` | **(ii)** Commit timestamps strictly increase; the `i`-th commit has timestamp `i+1`. |
| `Target.target_ro_snapshot` | **(ii)** Every read of an RO transaction at timestamp `n` returns the state after exactly the commits with `ts ≤ n`. |
| `Target.disjoint_never_abort` | **(iii)** Take two fresh RW transactions, from any state, in any interleaving. Suppose neither's read/validated key sets (`fp`) meet the key sets the other may write (`writeSpans`). Then no call of either returns `ABORTED`, so each commits unless its own operations raise a constraint error. |
| `Upstream.upstream_serializable` | **(iv-a)** Upstream's committed histories are serializable too, with strictly increasing timestamps. |
| `target_admits_upstream` | **(iv-b)** Run a schedule through Upstream and keep only the steps of the transactions it committed. Target, run on those steps, commits the same transactions in the same order, with the same reads, row counts, mutations and writes (the commit logs are equal). This needs `snapAtBegin = false`. |
| `Target.target_errors_latest`, `target_commit_errors_latest` | **(v)** Suppose a non-`ABORTED` constraint error comes back from a DML statement (with `validateErrors`) or from `Commit` (always). Then rerunning the transaction on the *latest committed state* raises the same error. This is stronger than serializability. |

Why (iv-b) filters the schedule: on the same unfiltered schedule, neither
model is more permissive. `target_differs_unfiltered`: Upstream commits T1
and aborts T2 because T2 touched the slot. Target lets T2 commit, then aborts
T1 because T2 wrote a row T1 read. Target accepts every history Upstream
commits; it just may not pick the same one.

## Design flaws found (`Counterexamples.lean`)

1. **The snapshot must not be taken at `BeginTransaction`**
   (`snapAtBegin_rejects`). T1 begins, T2 updates a row and commits, then T1
   reads the row and commits. Upstream commits both: T1 takes the lock only at
   its read. A snapshot taken at begin makes T1 read a stale value and fail
   validation. **Fix (modelled):** take the snapshot at the first data
   operation.
2. **Constraint errors against a stale snapshot must become `ABORTED`**
   (`naive_error_not_latest`). T1 fixes its snapshot while `A{1}` exists. T2
   deletes `A{1}` and commits. T1's `INSERT A{1}` sees the row in its snapshot
   and fails with `ALREADY_EXISTS`. That outcome is still serializable (T1
   before T2), but it reports a row that is already gone from the committed
   state, and clients treat constraint errors as final rather than retrying.
   Rerunning T1 on the latest committed state succeeds. **Fix (modelled):** on a constraint error from a
   data operation, validate the read set plus the failing operation's
   footprint. If stale, return `ABORTED`. `Commit` validates before applying
   its mutations, for the same reason.
3. **Read sets must record the key ranges scanned, not the rows returned**
   (`keysOnly_not_serializable`, a phantom). With rows returned, a
   concurrent insert into a scanned range goes undetected, and the committed
   history is not serializable. The design as specified (key ranges) aborts
   instead.

These examples show behaviour that is correct, but conservative:

* `aba`: delete-then-reinsert of the same value still aborts a reader.
  Validation compares versions, not values.
* `blind`: two `insert_or_update`s of the same row conflict, because mutations
  record an existence read of their row, as the emulator's flattening does.
  Last-writer-wins would be serializable too.
* Without `--pushdown`, any SQL or DML on a table conflicts with any write to
  that table. `disjoint_never_abort` takes both settings, so the gain from
  pushdown is the narrower `fp`.

## Not modelled

* **Commit is atomic.** Reserve, flush and `MarkCommitted` happen as one step,
  so the proofs say nothing about a read that overlaps a commit in flight.
  Phase 1 must keep two things true, both in how the timestamps are chosen:
  * An RW snapshot and a strong RO timestamp must not run ahead of commits
    that have reserved a timestamp but not finished flushing. Otherwise a read
    sees half a commit. Take the snapshot as the last *completed* commit
    timestamp under the commit mutex, or `WaitForSafeRead(ts)` as RO reads do
    today (`manager.cc:132-143`).
  * Validation and commit-timestamp reservation must be one critical section.
* **Wounding** (`--abort_current_transaction_probability > 0`), fault injection
  (`--enable_fault_injection`, `ShouldAbortOnFirstCommit`), and anything that
  blocks: the emulator never blocks, and neither model does.
* **Schema changes.** The emulator's exclusive DDL lock, and aborts from
  `AbortDueToConcurrentSchemaChange`.
* **Columns.** One non-key column; read sets are per row. A column-level read
  set must still count row existence as read by every read and written by
  every insert and delete.
* **Other things:**
  * Secondary indexes, interleaving, foreign keys, commit timestamps in data,
    change streams.
  * `REPLACE` mutations. `delete; replace; update` hits the
    `INVALID_ARGUMENT` path, because `replace` doesn't clear the deleted list.
  * Batch DML, partitioned DML, stale reads other than exact-timestamp (max
    staleness, min timestamp), multiplexed sessions, and inline `begin` in a
    `Read`/`ExecuteSql` selector.
  * Operations after `Commit`/`Rollback`, and on unknown transaction IDs. The
    generator never issues these.
* **Tombstone detail.** The emulator erases an insert-then-delete of a row
  that is absent from storage from its buffer. The model keeps a tombstone.
  This affects only Target conflict detection (it adds a write), never results.
* **Isolation level.** Only `SERIALIZABLE`; `REPEATABLE_READ` is out of scope.

## The implementation

The fork's emulator implements Target. Where each rule lives:

| Target rule | C++ |
|---|---|
| No lock slot; lock requests never block or abort | `LockHandle::EnqueueLock` records reads only (`backend/locking/handle.cc:41`) |
| Snapshot at the first data operation, not at `BeginTransaction` | `ReadWriteTransaction::AcquireSnapshot` (`backend/transaction/read_write_transaction.cc:343`), called from `Read` and `Write`; `LockHandle::SnapshotTimestamp` (`backend/locking/handle.cc:75`) |
| The snapshot is never ahead of a commit still flushing | `LockManager::PickSnapshotTimestamp` takes a fresh timestamp, then `WaitForSafeRead` waits for every pending commit at or before it (`backend/locking/manager.cc:56,73`) |
| Reads at the snapshot, overlaid by the buffer | `TransactionStore::ReadTimestamp` replaces `InfiniteFuture` in every storage read (`backend/transaction/transaction_store.cc:97`) |
| Read set = key ranges scanned (`fp`), not rows returned | `TransactionStore::AcquireReadLock` (`transaction_store.cc:77`), including `RowExistsInStorage` (`:370`); index, FK and interleave checks read through `Lookup`/`Read`, so they are covered |
| `fp (.commit ms)`: every mutation's row, before any is applied | `ReadWriteTransaction::RecordMutationReads` (`read_write_transaction.cc:354`) |
| Commit: validate, then take a timestamp, atomically | `LockHandle::Commit` (`backend/locking/handle.cc:124`): under the commit mutex, `ReadSetIsStaleLocked` (`:100`), `ReserveCommitTimestamp`, flush, `MarkCommitted` |
| `stale`: a commit after the snapshot wrote a row in the read set | `Storage::HasVersionsAfter` (`backend/storage/in_memory_storage.cc:238`) |
| Every write of a commit is a version, including a cancelled insert-then-delete | `TransactionStore::GetCancelledWrites` (`transaction_store.cc:448`) and `Storage::MarkWritten` (`in_memory_storage.cc:226`) |
| `errCode` / `target_errors_latest`: a constraint error on a stale read set is `ABORTED` | `AbortIfReadSetStale` for `Write` and `Commit` (`read_write_transaction.cc:409,504`); DML errors raised by the query engine, `MaybeAbortOnStaleReads` (`read_write_transaction.cc:422`, called at `frontend/entities/transaction.cc:240`) |
| Strictly increasing commit timestamps; RO reads unchanged | `LockManager::ReserveCommitTimestamp` (`manager.cc:40`); pending commits are a set, and a read at or after a pending commit's timestamp waits for it |
| `sqlSpan` with `pushdown` | `ComputeScanKeySets` (`backend/query/key_narrowing.cc:581`), set per statement at `backend/query/query_engine.cc:1431,1464`; flag `--enable_query_key_pushdown` (default on) |
| `fp (.dmlInsert ..)`: the inserted row | `InsertedKeys` (`key_narrowing.cc:401`), independent of the pushdown flag |

Beyond the model:

* **Schema changes** hold the commit mutex exclusively
  (`backend/database/database.cc:172`). They wait for a commit in flight but no
  longer fail because read-write transactions are open. Those abort on their
  next operation or at commit.
* **Partitioned DML and `BatchWrite`** run through the same `Commit`.
* **`--abort_current_transaction_probability`** now aborts that percentage of
  read-write commits at random, for testing retry loops (default 0,
  `common/config.cc:47`). The gateway forwards it and
  `--enable_query_key_pushdown` to `emulator_main`.
* **Pushdown also serves GoogleSQL column filters**
  (`EvaluatorTableIterator::SetColumnFilterMap`,
  `backend/query/queryable_table.cc:95`) for scans the statement analysis
  leaves alone, such as a table scanned twice. Column filters are inclusive,
  so those key sets can be wider than the predicate, never narrower.
  Contradictory bounds read nothing rather than an inverted range
  (`key_narrowing.cc:149`).
* **Columns**: read sets are per row, like the model; a write to any column
  of a row conflicts with any read of the row.
* **Pending commits** are a set; a read waits while its minimum is at or
  before the read timestamp. Commits run one at a time under the commit
  mutex, so it holds at most one entry.
* **Schema changes vs transactions**: a schema change can publish newer
  schemas and garbage-collect old ones while transactions still use them, so
  everything that uses a schema owns it:
  * A read-write transaction owns its schema and action registry. Before its
    first data operation it hands out the latest schema and keeps it; that
    operation runs on it, or aborts if a schema change replaced it after it
    was handed out.
  * A read-only transaction owns the schema at its read timestamp.
  * Read cursors own the schema their columns belong to.
  * Sequence and time zone functions evaluate against the latest schema, as
    upstream does, and hold the one they loaded until they are done.
  * The process-wide PostgreSQL system catalog takes only schemas its source
    catalog owns, never a schema change's intermediate schema, which is
    destroyed when the change finishes.
  * Admin handlers (`GetDatabaseDdl`, instance partitions) hold the latest
    schema through `Database::GetLatestSchemaShared`.

  Whole schema changes are serialized by their own mutex, which also orders
  change stream churner updates.

One deviation from the model (the model has no such columns):

* A mutation whose key column has a default or generated value cannot be put
  in the read set before the mutations are applied, because evaluating the key
  can have side effects (sequences). Its row joins the read set when the
  mutation is flattened. If an earlier mutation of the same `Write` or
  `Commit` fails first, the error is validated against the whole table instead
  (`ReadWriteTransaction::AbortIfReadSetStale`). So the error becomes
  `ABORTED` whenever validating the row would have made it `ABORTED`, and also
  when a commit after the snapshot wrote any other row of that table.

### Stress test

`verification/stress` runs 16 goroutines through the Go client
(`cloud.google.com/go/spanner` v1.88, whose read-write transactions use
multiplexed sessions) with its retry loop, 15 s per workload, and checks every
balance against the committed transfers:

* `contended`: transfers among 4 accounts, written with `UPDATE ... WHERE
  Id = @id`.
* `disjoint`: each worker transfers between its own 2 accounts.
* `mux282`: inserts through an explicit `BeginTransaction` plus buffered
  mutations (the pattern of upstream issue #282, whose writes were silently
  lost), then checks that every committed row exists.

```sh
cd verification/stress
SPANNER_EMULATOR_HOST=localhost:19011 mise exec go@1.25 -- go run . -workers 16
mise exec go@1.25 -- go run . -image gcr.io/cloud-spanner-emulator/emulator:1.5.58 -abort-probability 0
```

| Emulator | Workload | Committed txn/s | Aborted attempts per commit |
|---|---|---|---|
| fork, native, pushdown on | contended | 498 | 0.81 |
| | disjoint | 4,068 | **0** |
| | mux282 | 10,740 | **0** |
| 1.5.58 image, probability 0 | contended | 55 | 1.55 |
| | disjoint | 105 | 0.89 |
| | mux282 | 279 | 7.59 |
| 1.5.58 image, probability 20 | contended | 209 | 1.23 |
| | disjoint | 146 | 1.35 |

Every run kept the total balance and every account's balance, and lost no
committed row. With `--abort_current_transaction_probability=20`, the fork's
disjoint workload aborts 19.9% of attempts, all injected. The 1.5.58 figures
come from its Linux image under Docker on the same machine, so they include
the VM's overhead. Its `mux282` run at probability 20 did not finish: after
the writers stopped, a strong read of one row blocked for more than ten
minutes (not diagnosed further).

Issue #282 does not reproduce on the fork: 161,111 concurrent #282-pattern
commits, none lost.

## Running

### Proofs and the model executable

```sh
cd verification/lean
lake build                      # proofs + txnmodel; fails on any error
grep -rn sorry TxnSpec          # must print nothing
.lake/build/bin/txnmodel --model upstream < schedule.jsonl
.lake/build/bin/txnmodel --model target [--pushdown] < schedule.jsonl
```

`txnmodel` reads one step per line and prints one result per step:
`{"step":i,"txn":t,"res":R}`. `--batch` reads a whole schedule (a JSON array
of steps) per line and prints a JSON array of results per line. The harness
uses batch mode.

Step format:

```json
{"txn":1,"op":"begin_rw","args":{}}
{"txn":2,"op":"begin_ro","args":{"at":1}}           // omit "at" for strong; clamped to commits so far
{"txn":1,"op":"read","args":{"table":"A","keys":{"point":3}}}
{"txn":1,"op":"sql","args":{"table":"B","keys":{"lo":1,"lo_incl":true,"hi":4,"hi_incl":false}}}
{"txn":1,"op":"dml_insert","args":{"table":"A","key":2,"val":7}}
{"txn":1,"op":"dml_update","args":{"table":"A","keys":"all","val":9}}
{"txn":1,"op":"dml_delete","args":{"table":"A","keys":{"point":2}}}
{"txn":1,"op":"commit","args":{"mutations":[{"kind":"upsert","table":"A","key":1,"val":5}]}}
{"txn":1,"op":"rollback","args":{}}
```

Mutation kinds: `insert`, `update`, `upsert`, `delete` (`val` ignored).
Results: `"ok"`, `{"rows":[[k,v],…]}`, `{"count":n}`, `{"committed":i}` (the
`i`-th commit of the schedule), `{"error":"ABORTED"}`.

### Conformance

The harness runs `emulator_main` directly, with
`--abort_current_transaction_probability=0`. Against the fork, its
`--enable_query_key_pushdown` must agree with the harness's `-pushdown`:

```sh
cd verification/conformance
(cd ../lean && lake build)                           # builds txnmodel
# the fork, built natively (docs/building-on-macos.md):
bazel-bin/binaries/emulator_main --host_port localhost:19010 \
  --abort_current_transaction_probability=0 --enable_query_key_pushdown=false &
SPANNER_EMULATOR_HOST=localhost:19010 mise exec go@1.25 -- go run . -seeds 3000
# with --enable_query_key_pushdown=true (the default):
SPANNER_EMULATOR_HOST=localhost:19011 mise exec go@1.25 -- go run . -pushdown -seeds 3000
# upstream 1.5.58 against the Upstream model; starts the image itself:
mise exec go@1.25 -- go run . -model upstream -seeds 2000
```

Useful flags:

| Flag | Effect |
|---|---|
| `-model target` (default) | Target must match; exit 1 on any mismatch. |
| `-model upstream` | Upstream must match. Only upstream 1.5.58 does; the fork diverges by design. |
| `-pushdown` | Target with SQL key predicates narrowing the read set. |
| `-must-match=false` | Report divergences without failing. |
| `-image` | The emulator image to start when `SPANNER_EMULATOR_HOST` is unset. |
| `-print -first-seed N` | Print seed `N`'s schedule. |
| `-schedule file.jsonl` | Replay one schedule and print the step-by-step diff. |
| `-dump file.jsonl` | Save the shrunk failing schedule. |

Each schedule runs from one goroutine, so the interleaving is exactly the
schedule order:

1. A setup transaction (txn 0) clears both tables and inserts random rows.
2. Then 2–4 concurrent transactions (20% RO), keys 0–5, in a random
   interleaving. Each RW transaction runs up to 4 data operations and commits
   up to 6 mutations. Half of the mutations reuse a row from earlier in the
   commit. One commit in five also churns a row: one or two deletes, a
   re-insert (`insert` or `upsert`), and sometimes an update.
3. A final strong RO transaction reads every table in full. Every committed
   write is observed, including writes after the schedule's last read.

Each transaction gets its own session, because a session closes its earlier
transactions when a later one is used (`session.cc:171,272`). The harness also
checks that the emulator's commit timestamps strictly increase. On a mismatch,
it shrinks the first failing schedule by dropping transactions, operations and
mutations, then prints it.

## Results

### The fork

`emulator_main` built natively on macOS arm64 from this branch,
`--abort_current_transaction_probability=0`, seeds 1–3000:

| Model | `--enable_query_key_pushdown` | Schedules | Steps | Mismatching schedules | `ABORTED` (emulator / model) |
|---|---|---|---|---|---|
| `target` | `false` | 3000 | 49,332 | **0** | 726 / 726 |
| `target -pushdown` | `true` | 3000 | 49,332 | **0** | 607 / 607 |

Upstream mode now diverges, as intended. Seeds 1–1000 against `-model
upstream`: 535 of 1000 schedules mismatch; the emulator aborts 225 times, the
Upstream model 2,269. At every first divergence the Upstream model aborts for
the lock slot where the fork proceeds. Crossing the pushdown settings
(emulator with pushdown, Target model without) mismatches 36 of 1000
schedules. At each, the model aborts where the emulator, whose read set is
narrower, commits or reports a constraint error. So the runs above do
exercise pushdown.

### Upstream 1.5.58

Emulator `gcr.io/cloud-spanner-emulator/emulator:1.5.58`, `emulator_main
--abort_current_transaction_probability=0`, seeds 1–2000:

| Model | Schedules | Steps | Mismatching schedules | `ABORTED` (emulator / model) |
|---|---|---|---|---|
| `upstream` | 2000 | 32,854 | **0** | 4,565 / 4,565 |
| `upstream`, seeds 2001–6000 | 4000 | 65,178 | **0** | 8,815 / 8,815 |
| `target` | 2000 | 32,854 | 1,104 | 4,565 / 479 |
| `target --pushdown` | 2000 | 32,854 | 1,104 | 4,565 / 396 |

Upstream matches every step. In every one of the 1,104 Target divergences,
the first diverging step is the emulator returning `ABORTED` where Target
proceeds. That was the gap the fork closes: 1.5.58 aborts transactions because of
the single lock slot, and Target does not. Sometimes the Target outcome that
first differs is a real constraint error. For example, a commit the emulator
aborts for the lock gets `ALREADY_EXISTS` under Target because the row exists;
by `target_commit_errors_latest`, rerunning on the latest committed state
raises that same error.

**Regression caught in review:** repeated deletes of one key recorded it twice
in the deleted-key list, and re-insert cleared only one record. The emulator
clears the key from every deleted range. Fixed in `Local.reinsert`, with a
regression schedule in
`testdata/regression_repeated_delete_reinsert.jsonl` and a `decide` example in
`Counterexamples.lean`. The generator now produces the pattern: run against
the old model, it fails 65 of 2000 schedules and shrinks to
`delete B{0}; delete B{0}; insert B{0}; update B{0}`.

**Upstream bug found:** a RW `Read` of an empty open range such as `(2,2)`
crashes the emulator process (SIGSEGV, exit 139), once the transaction has
buffered a write in that table. `KeyRange::ToClosedOpen` turns `(k,k)` into
`[succ(k), k)`, with start after limit (`backend/datamodel/key_range.cc:126-140`).
`TransactionStore::Read` then walks the buffered ops from `lower_bound(start)`
until it reaches `lower_bound(limit)`, which is earlier in the map, so it runs
off the end (`backend/transaction/transaction_store.cc:256-259`). Reproduce
with:

```sh
go run . -schedule testdata/upstream_crash_empty_open_range.jsonl
```

`testdata/regression_*.jsonl` are schedules that must match; replay each with
`-schedule`.

The generator avoids empty ranges so the rest of the run can proceed.

## L2: persistence protocol and crash conformance

L2 changes `verification/` only. It specifies a storage protocol and tests a
crash/restart driver; it does not implement emulator persistence. The L1
transaction models and proofs remain unchanged.

| File in `lean/TxnSpec/` | Purpose |
|---|---|
| `Persistence.lean` | One physical WAL, checkpoints, durable clock leases, crash transitions and invariants. |
| `PersistenceSequences.lean` | Storage/sequence allocator product and uniqueness proof. |
| `PersistenceComposition.lean` | Storage/Target product, recovered row equality and physical timestamp correspondence. |
| `PersistenceCounterexamples.lean` | Reachable faulty protocol traces, non-vacuity examples and axiom audit. |
| `RestartJson.lean` | Executable oracle and its Target invariant proof. |

### Storage protocol and assumptions

`Image` contains catalog incarnations, a catalog allocator, rows, opaque schema
metadata, table/column allocators, sequence reservation ends and the last
committed timestamp. A transaction record contains resolved physical writes;
a DDL record contains schema and resolved backfill writes together. Replay
does not evaluate SQL, defaults, generated expressions or backfills. Physical
index entries can use separate table IDs. The catalog allocator advances on
CREATE, deliberately making redo non-idempotent: recovery must respect the
checkpoint boundary, not assume that repeating covered operations is harmless.

`Disk.physical` is the **one physical WAL**, containing `(sequence, record)`
entries. `Disk.restore` replays the checkpoint image plus
`physical.filter (sequence >= checkpoint.boundary)`. Truncation removes only
entries below a chosen cutoff at or below that boundary; multiple partial
truncations and crashes can interleave. `physical_recovery` proves this equals
replaying the logical suffix and the full logical history for every reachable
state. `wal`, `Snapshot.past`, product histories and acknowledgement/read lists
are **ghost proof fields**, not additional recovery files. `Disk.restore`
never reads these ghost fields.

The append protocol is `begin → finish → fsync → flush → ack`, with optional
repeated torn-byte writes before `finish`. `fsync` moves a complete record
into the durable physical WAL. Alternatively `persist` models an early durable
writeback before fsync returns, followed by `syncPersisted`. A crash may retain
that complete unacknowledged record; a complete record that has not reached
durable storage is lost. `flush` installs the whole resolved effect atomically.

**The proved global gate is held from `begin` through `ack`.** Target calls,
including reads and validation, wait until the gate is idle. Validation at
`begin` stages a candidate; the Target commit takes effect at `Move.flush`.
Checkpoint capture also requires an idle gate, and `quiescent_image` proves
that the captured live image matches the log boundary. Phase 2 must implement
this gate or supply a proof for a weaker protocol. Global timestamp order is
sufficient, not necessary: per-database prefixes plus ordering of catalog
operations could establish a different recovery theorem. A single global gate
is the simpler choice for a development emulator.

Checkpoint scratch is volatile and publication is atomic and durable after
snapshot fsync. `checkpointBytes` is a **stuttering abstraction**: partial
snapshot byte writes do not affect the durable state until publication. There
is no byte-level partial-snapshot proof. Concurrent appends after capture are
permitted; publication preserves the physical WAL, changes its replay boundary,
and leaves the newer tail active. `checkpoint_safety` uses the same durability,
ordering and physical-restore invariants after each transition.

A durable clock lease stores an upper bound `L`. `serveRead` and `ack` can
expose timestamps only up to `L`; extending the lease is a durable operation.
Restart sets its clock strictly above the lease, checkpoint clock, recovered
commit timestamps and wall time. `served_within_lease`, `acknowledged_within_lease` and
`every_post_restart_timestamp` prove that **every** subsequent commit reservation
in the live epoch exceeds every read timestamp served before the crash, even
when no checkpoint occurred.

The sequence allocator exposes only installed durable reservations. Crash
abandons the unused remainder and resumes at the exclusive durable upper
bound. `allocator_refines` and `storage_sequences_never_reissue` cover every
storage transition, queries and aborted transactions. Uniqueness is per
incarnation: implementation IDs must not be reused, any output encoding must
be injective, and finite counters must fail rather than wrap.

Recovery rejects detected non-tail corruption. Ignoring a torn final record
assumes correct framing/checksums. **Clearing `tornTail` on restart obliges the
implementation to physically truncate the torn bytes before another append.**
The small `corruption_fails` and `pre_restart_read_rejected` lemmas merely unfold
policy definitions; they are not credited as substantive verification results.

### Theorem map

`lake build` checks these statements without admitted proofs or `native_decide`.
Printed dependencies are subsets of the core axioms `propext`, `Quot.sound`
and `Classical.choice`.

| Contract | Theorem(s) | What is proved |
|---|---|---|
| (a) Durability | `durability`, `restore_eq` | Acknowledged records remain in the durable logical history, and physical recovery equals its replay. Later effects may supersede earlier ones. |
| (b) Atomicity/order | `atomicity`, `log_monotone`, `physical_recovery` | Recovery includes whole records in strict timestamp order; every storage transition extends or preserves the logical log. |
| (c) Checkpoints | `checkpoint_safety`, `physical_recovery`, `quiescent_image` | Captured live data is correct; publication, partial prefix deletion and arbitrary crashes preserve (a)/(b). No idempotence assumption is needed. |
| (d) Timestamps | `served_within_lease`, `every_post_restart_timestamp` | All post-restart commit reservations exceed pre-crash served read timestamps, saved/recovered timestamps and wall time. |
| (e) Sequences | `storage_sequences_never_reissue`, `allocator_refines` | No reissue in the coupled storage/allocator machine. |
| (f) Drops | `acknowledged_drop_recovered`, `drop_record_survives`, `instance_drop_survives` | An acknowledged DROP with no later CREATE of that incarnation is absent in physical recovery. The instance lemma covers owned databases. |
| (g) Composition | `product_target_inv`, `product_recovered_rows`, `product_recovered_target_rows`, `product_visible_rows`, `product_timestamp_mapping`, `target_serializable_across_restarts` | Every reachable product projects to `Persistence.Reachable`, `AcrossRestarts` and `Target.Inv`; physical recovered rows equal `stateAt`, and ordinal/physical timestamps have the same ordered correspondence. |
| Executable oracle | `modelStep_target_across`, `runRestartState_target_across`, `runRestartModel_target_inv` | Every executable schedule prefix preserves `AcrossRestarts`/`Target.Inv` for the Target model. |

The composition is for **one fixed-schema Target instance** (database 0).
After boot, the product appends transaction records only; DDL, catalog and
sequence records are covered by the separate storage/allocator proofs, with
no such interleaving in the Target composition. There is no proof of concurrent
SQL schema validation or a multi-database Target composition. During the
persisted-before-flush window the live Target remains unchanged; recovery uses
the staged durable candidate. `ProductMove.crash` performs `Move.crash` and
selects that candidate only when the storage phase is durable. After crash,
restart or any idle state, `product_recovered_target_rows` equates recovery
with the ordinary Target log. No open transaction survives recovery.
Product Target reads are plain `call`s that bypass `serveRead`/the clock lease
and allow old exact ordinal reads after restart: this deliberately more
permissive serializability product does not prove the serving policy enforced
by the storage clock model and executable restart oracle.

`product_timestamp_mapping` proves ordinal timestamps `1..n`, the correspondence
to each physical record, and strictly increasing physical timestamps.
`target_serializable_across_restarts` follows from the product invariant;
it does not assume storage history survival. Durable uncertain commits belong
to this history even when their client reply was lost. Exactly-once retries
and service of old MVCC snapshots are not promised.

### Reachable broken variants

These are traces in `BrokenMove`/`BrokenReachable`, with normal `Move` transitions
plus the named faulty transition; the cursor case uses `BrokenSequenceMove`.
The three strengthened witnesses below explicitly contradict the named
invariant; each also has a lemma excluding the same violation in the correct
reachable machine. A stopped process, a read served after a commit, or a
pending flush cannot stand in for a recovery violation.

| Fault and reachable trace | Refutation | Required fix |
|---|---|---|
| Finish bytes, acknowledge, crash without persistence | `early_ack_loses_durability` | Fsync/install before acknowledgement. |
| Capture snapshot, truncate to its unpublished boundary, crash/restart with `restore ≠ replay log` | `early_truncate_loses_recovery` (violates `restore_eq`/`physical_recovery`) | Delete only below the published boundary. |
| Publish, crash, replay all retained physical WAL atop image | `covered_wal_replay_is_wrong` | Filter by sequence boundary; catalog allocator would otherwise advance twice. |
| Lease 10, serve read 7, crash; the wall-only restarted epoch permits reservation 3 ≤ 7 | `wall_restart_regresses` (violates the served-read clause of `every_post_restart_timestamp`) | Restore floor from durable lease and recovered clock. |
| Reserve, issue 0, reset cursor, issue 0 again | `cursor_reset_reissues` | Restart at durable reservation end. |
| Persist resolved backfill 7, crash, replay then re-execute SQL; live idle image differs from replay (8 versus 7) | `rerun_backfill_changes_rows` (violates `quiescent_image`) | Replay resolved writes only. |
| Reserve 2, another database appends 3, first appends 2 | `per_database_gate_reorders_wal` | Hold the shared gate for the global-order theorem; this example does not refute all per-database protocols. |

`correct_recovery_excludes_early_truncate`,
`correct_restart_excludes_regression` and `correct_quiescence_excludes_rerun`
prove that the respective violations cannot occur with the correct protocol.
The clock witness and its exclusion use the same `PostRestartRegression`
predicate, relating pre-restart served reads to reservations in the new live
epoch; the witness changes only the recovery clock rule.

Witness `example`s exhibit nonempty acknowledgements, a published boundary
above zero, a crash/restart, and a nonempty Target product history surviving
its actual storage crash.

### Crash driver and schedule format

All L1 steps remain valid. The separate restart envelope accepts:

```json
{"txn":2,"op":"crash","args":{"during_commit":true,"delay_us":500,"mutations":[{"kind":"upsert","table":"A","key":1,"val":7}],"durable":false}}
{"txn":0,"op":"restart","args":{}}
```

Omit `during_commit` for a between-call crash. The driver dispatches Commit,
waits for its gRPC payload or response, delays, and SIGKILLs its own container.
In persistent mode success forces the durable oracle branch; a transport
interruption explores both lost/durable outcomes. A terminal transaction error
sets `terminal: true`: the model **executes the commit and must return the same
error**. It is not silently converted to `ok`. The emulator driver clears supplied outcome hints and derives them from RPC
observations. Direct `txnmodel` callers can select `durable`/`terminal` to replay a chosen observation. Whole schedules must
match a single branch, with at most eight ambiguous commits (256 branches).

While stopped, calls fail with `FAILED_PRECONDITION`. Restart loses open
transactions. Persistent mode retains committed history and rejects exact
pre-restart snapshots. No-persistence mode empties data. Successful explicit
commit results use acknowledgement indices; hidden crash-commit successes do
not consume those indices.

Generated schedules perform exact reads before the crash, attempt a saved exact
timestamp after restart, and scan both recovered tables before later writes.
The `old` flag on `begin_ro` preserves the physical pre-crash timestamp even
when no-persistence mode clears its acknowledgement indices. Later generated
transactions use strong reads. The additive `RestartAudit` table stores a
commit-timestamp column in the **same commit** as user mutations. After restart,
the driver reads it so a durable lost-ack commit raises the floor for later
commit timestamps. Served read timestamps and successful commit replies also
raise that floor. This modifies the workload: every Commit receives an audit
write, and the first Commit of each schedule also deletes all audit rows.
Consequently these conformance numbers describe the audit-modified crash
workload, not the uninstrumented L1 workload or its lock/conflict behavior.
Audit keys are hex-encoded explicit transaction IDs. Single-use commits are
not generated or supported by this wrapper: their empty transaction IDs would
share one audit row, so support would require distinct audit keys first.

`TestPersistentCrashDriver` runs the real `runCrashSchedule` against a fake gRPC
emulator; only Docker lifecycle commands are substituted. Its eight cases check
READY polling, candidate fan-out, forced durability for successful replies,
accepted full/lost ambiguous commits, rejected partial commits, rejected loss
after success, rejected lost-ack timestamp regression, and exact terminal errors.
This is driver/oracle testing, not conformance of a persistent emulator.

```sh
cd verification/lean
lake build
cd ../conformance
mise exec go@1.25 -- go vet ./...
mise exec go@1.25 -- go test -race -count=1 -v ./...
mise exec go@1.25 -- gofmt -l *.go
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 500 -first-seed 1
# Phase 2, when an existing persistence-capable image is available:
mise exec go@1.25 -- go run . -persistence persistent -model target -image YOUR_PHASE2_IMAGE -seeds 500
```

The crash driver creates one container with `--cpus=2`, an ephemeral loopback
port excluding 9010/9020, and its own temporary mounted `/data`. It restarts the
same container/mount/port, verifies exit 137 after each SIGKILL, waits for READY,
and cleans up only its own resources. It refuses `SPANNER_EMULATOR_HOST`.
No-persistence mode requires the old database to be absent before setup;
persistent mode requires the old database to recover and never replaces it.
Run conformance serially using the existing image; do not build Docker images.
`-print`, `-schedule` and `-dump` support reproduction. The L1 shrinker does not
remove lifecycle steps from crash schedules.

`-checkpoint-command` can request an asynchronous checkpoint before a
between-call kill in Phase 2. Upstream has no such trigger, so this run has
zero implemented checkpoint tests. Timed SIGKILL cannot identify an internal
instruction. **Container SIGKILL leaves the kernel page cache alive: it cannot
detect missing/misordered file fsync or directory fsync.** Phase 2 also needs a
fault-injecting filesystem, such as LazyFS, and stage-specific hooks. The Lean
model assumes the durability and atomic-publication primitives it specifies;
it does not prove filesystem behavior, byte encoding, checksum correctness or
the C++ implementation's refinement. PostgreSQL, change streams, LRO history
and backups remain out of scope.

### L2 conformance results

The recorded serial run at `4bbabb73` against upstream 1.5.58 passed **500 distinct seeds,
15,508 steps, 0 mismatches and 500 SIGKILL/restarts**. Its 254 crash-commit
attempts included 116 interrupted RPCs, 33 successful replies and 105 matching
terminal errors. There were no checkpoint attempts. The clean Lean build and
core-axiom audit and Go vet/formatting/race tests passed again for this revision.
Driver behavior and the executable oracle are unchanged (the only Go edit is
a comment), so the 500-seed Docker gate is not rerun for round 2.

See the [review-response report](results/l2-report.md) for exact commands, raw
logs, theorem dependencies and point-by-point review responses. The earlier four-shard logs are historical evidence for
`f38776c` and are not counted toward the revised-code gate.

## Persistence implementation

The fork implements the L2 protocol behind `--data_dir`
([docs/persistence.md](../docs/persistence.md)). Without the flag none of it
runs: no gate is taken, no file is touched, and commits flush to storage as
before.

| Model (`Persistence.lean`, `PersistenceSequences.lean`) | C++ |
|---|---|
| One physical WAL of `(sequence, record)` entries (`Disk.physical`) | `frontend/persistence/log.cc`: segment files `wal-<first LSN>.log`; every frame holds its LSN, length and CRC32C; `Log::Open` rejects a gap in LSNs |
| Records hold resolved physical effects; replay evaluates no SQL | `backend::StorageOp` (table/column IDs, keys, values after defaults, generated columns and commit timestamps are resolved), recorded by `RecordingStorage`; a schema change's record holds its schema and its backfill writes (`Database::ApplySchemaChangeLocked`); recovery writes them straight into storage (`LoadRows` in `frontend/persistence/manager.cc`) |
| `begin → finish → fsync → flush → ack` under one global gate; records in commit-timestamp order across databases | `LockHandle::Commit` (`backend/locking/handle.cc`) takes the emulator-wide gate after validation and holds it from `ReserveCommitTimestamp` to `MarkCommitted`; inside, `ReadWriteTransaction::Commit` records the writes, `LogCommit` appends and syncs them, and only then are they applied. Catalog records take the gate and a clock timestamp (`PersistenceManager::Log*`) |
| No `ack` without `fsync` (`early_ack_loses_durability`) | a failed write or sync fails the commit before its writes are applied, and marks the log broken so every later record fails (`Log::Append`) |
| Torn final bytes are never replayed, and are truncated before the next append | `ScanSegment`: a damaged record with no intact record after it, in the last segment, is dropped and the file truncated in `Log::Open` |
| Detected non-tail corruption fails recovery (`Disk.recover`) | any other damaged record, a damaged file header or checkpoint, or a missing segment is `DATA_LOSS`; an unknown format version is `FAILED_PRECONDITION` (version 2 is written; version 1, with int64-nanosecond timestamps, is still read and never appended to) |
| `checkpointStart` needs an idle gate; `quiescent_image` | `PersistenceManager::Checkpoint` holds the gate while it starts a segment (boundary = next LSN) and copies instances, schemas (owned, `GetPersistedSchema`), rows and sequence reservations; a database it cannot reach fails the checkpoint rather than being left out, and `ServerEnv` stops the checkpoint thread before it destroys the databases |
| `publish` is atomic and durable | `Log::PublishCheckpoint`: temporary file, sync, rename, directory sync |
| `truncate upto ≤ checkpoint.boundary` (`early_truncate_loses_recovery`) | `Log::RemoveSegmentsBelow` removes whole segments below the published boundary, never past it |
| `restore` = checkpoint image + records with sequence ≥ boundary (`covered_wal_replay_is_wrong`) | `PersistenceManager::Recover` skips records below the boundary; recreating a database uses a fresh incarnation, so replaying a covered CREATE would not be idempotent either |
| Clock lease: `serveRead` and `ack` stay within the durable lease; restart above `max(clockHigh, lease, recovered clock, wall)` (`wall_restart_regresses`) | `Clock::SetLease`: no timestamp past the lease is handed out before a lease record is synced; a caller-chosen read timestamp is covered before a read-only transaction exists (`Clock::CoverWithLease` in `Database::CreateReadOnlyTransaction`); the checkpoint stores `max(clock, lease)`; `Recover` calls `Clock::AdvanceTo`; a chosen read timestamp after 9999-12-31T22:59:59.749999999Z would leave no room below Spanner's maximum once the clock restarts above its lease, so `CoverWithLease` refuses it (`INVALID_ARGUMENT`), and the codec refuses to write an out-of-range timestamp |
| `readAllowed boundary ts`; pre-restart exact reads rejected | `Database::SetRestartFloor`; `Database::CreateReadOnlyTransaction` returns `FAILED_PRECONDITION` below it; bounded staleness never picks a timestamp below it (`LockManager::AdvanceLastCommitTimestamp`) |
| Sequence allocator issues only below durable reservations; a crash resumes at the durable end (`cursor_reset_reissues`) | `Sequence::GetNextSequenceValue` calls the reservation hook, which syncs a reservation record, before handing out a value past the reserved end; `Sequence::RestoreReservation` |
| Drops: a dropped incarnation is never recreated | databases and instances have incarnation numbers; records of a dropped incarnation are ignored on replay |
| Backfills are not re-run (`rerun_backfill_changes_rows`) | recovery rebuilds each schema from its DDL against empty storage, then loads rows; no default, generated column or backfill is evaluated |

Beyond the model:

* **Schemas are stored as DDL** plus the storage IDs of their tables,
  columns and sequences. Recovery rebuilds the schema through the normal
  `Database::Create` path with those IDs preassigned
  (`UniqueIdGenerator::Preassign`, `SchemaChangeContext::sequence_ids`) and
  fails if the rebuilt IDs differ. The DDL is printed with foreign keys added
  after every table (`PrintDDLStatements(schema, /*foreign_keys_last=*/true)`),
  because a parent can reference its interleaved child, a cycle the
  inline form cannot recreate. The Stelaxis smoke test found this.
* **Only GoogleSQL databases** are persisted; PostgreSQL-dialect creation is
  refused with `--data_dir`.
* **Values** use the client wire encoding with their own type, so a value
  decodes without its column's current schema.

### Deviations from the model

1. **Sequence reservations and clock leases have no timestamp and do not take
   the gate.** Their effect is a maximum, which commutes with every other
   record, so their position in the log does not matter. Taking the gate
   would deadlock: a schema change holds it while its backfill draws
   sequence values. The model appends every record under the gate with a
   timestamp.
2. **Leases are log records.** The model's `Disk.lease` is a separate durable
   field. Truncating the log can drop old lease records, so each checkpoint
   stores `max(clock, lease)` as its clock high-water mark.
3. **A schema change's backfill writes reach storage before its record is
   synced**, because backfills read their own writes. They stay invisible
   until the change is marked committed (reads at or after its timestamp
   wait). The new schema itself is published to the versioned catalog, which
   every reader of the latest schema (`GetDatabaseDdl`, queries, new
   transactions) goes through, only after the record is synced, as in the
   model's `flush` after `fsync`. Every check that installing the schema
   makes (`VersionedCatalog::CheckSchema`: timestamp order, retention period)
   runs before the record is written, so a logged schema is always installed.
   A change rejected as a whole (by the updater or by `CheckSchema`) is
   rolled back in storage (`Storage::RollBackVersionsAt` removes the
   versions and drop marks at its timestamp) and not logged. That also
   changes the in-memory behavior: upstream 1.5.58 kept a rejected type
   change's converted values under the old column type, and a later
   comparison on the column crashed it (SIGSEGV). Sequence counters are forgotten only once the published
   schema lacks them, including sequences created and dropped within the same
   change. A schema change is refused if the log is already broken, but if
   its own record fails the backfill writes cannot be undone, so the emulator
   exits instead (`std::abort`), as it does if a logged schema could not be
   installed; a restart recovers what the log holds. Commits are logged
   before they are applied and need no such rule.
4. **The gate is taken after validation.** Validation reads only its own
   database, and that database's commit mutex already orders it with that
   database's commits and schema changes. The model takes the gate before
   validation.
5. **Segments.** The one physical WAL is split into files so that truncation
   removes whole files below the published boundary.
6. **No group commit.** Each commit syncs its own record, holding the gate.
7. **Multiple databases.** The composition proof covers one fixed-schema
   database; the implementation relies on one global timestamp order for all
   databases and the catalog.

### Tests and tools

* `frontend/persistence/log_test.cc`: the log on a simulated file system
  (`MemFileSystem`) that loses unsynced bytes and directory entries on a
  crash: a torn tail at every byte offset of a record, failed write and sync
  (not acknowledged, log broken), an unacknowledged record that reached the
  disk, corruption mid-log and in an earlier segment, a missing segment,
  unknown format versions, the lock, and crashes between checkpoint write,
  rename, directory sync and truncation.
* `frontend/persistence/persistence_test.cc`: the managers and recovery on the
  same file system: schemas and rows (storage dumps and storage IDs) across a
  crash, including an interleaved parent referencing its child; sequences
  never reissuing across four crashes; pre-restart reads refused; timestamps
  above everything before a crash even when the system clock steps back an
  hour; drops staying dropped; PostgreSQL refused; failed sync; a crash at
  every checkpoint stage (each fault targeted by file and checked to fire);
  concurrent commits with checkpoints; a corrupt log. From review: a failed
  `DROP SEQUENCE` keeps the sequence's state; a checkpoint fails rather than
  skip an unreachable database, and teardown never checkpoints one away; a
  future read timestamp returned to the client is covered across a crash; a
  schema change is invisible while its record's sync is blocked; recovery
  from the log alone ignores values of dropped proto and enum columns. From
  the second review: a schema change rejected when it is installed is not
  logged; a read timestamp in the year 3000 is covered across a crash;
  sequences created and dropped in one change leave no state; a version 1
  data directory is recovered, its timestamps included. From the third
  review: a rejected type change leaves its column's values and type alone,
  in memory and after a restart; a rejected change's drop marks are removed;
  read timestamps too close to the maximum are refused and the largest
  accepted one leaves the restarted clock usable; the codec refuses
  out-of-range timestamps; a migrated database's create time moves to the
  current field.
* The crash driver runs `emulator_main` natively with `-emulator-binary`
  (below). `-checkpoint-command 'kill -USR1 $EMULATOR_PID'` requests a
  checkpoint before a between-call kill.
* `stress -workloads crash -emulator-binary ... -data-dir DIR` kills the
  emulator with SIGKILL in the middle of the transfer workload and restarts
  it. Every transfer also inserts a `Transfers` row, so in-flight commits can
  be resolved: in one snapshot right after the restart, and again at the end,
  every balance must equal its initial value plus the transfers present,
  every transfer acknowledged before the kill must be present, and no
  transfer may be present that was never attempted.
* `stress/smoke` applies a schema file one statement at a time (foreign keys
  last), fills every table it can with type-driven DML, kills the emulator
  with SIGKILL, restarts it, and compares `GetDatabaseDdl` and every row, then
  checks that sequences continue without reissuing.

```sh
cd verification/conformance
go run . -persistence persistent -model target -pushdown \
  -emulator-binary ../../bazel-bin/binaries/emulator_main -port 19210 \
  -seeds 500 -checkpoint-command 'kill -USR1 $EMULATOR_PID'
cd ../stress
go run . -emulator-binary ../../bazel-bin/binaries/emulator_main \
  -data-dir /tmp/stress-data -port 19220 -workloads crash -duration 20s
go run ./smoke -emulator-binary ../../bazel-bin/binaries/emulator_main \
  -schema .../apps/stelaxis/priv/repo/structure.sql
```

### Persistence results

All on macOS arm64, native builds of this branch with `-c opt --jobs=6
--local_resources=cpu=6 --local_resources=memory=16384`; the Go gates ran
against the `emulator_main` built from the same tree.

| Gate | Result |
|---|---|
| Upstream suite: `//backend/... //common/... //frontend/... //gateway/... //binaries/... //tests/conformance/...` | 134 test targets: 132 pass. The 2 failures are on #2's known macOS list: `change_stream_backfill_test` and `PGFunctionsTest.ToJsonB` (1 of 32 `emulator_conformance_test` shards) |
| New and changed tests, `--runs_per_test=20` | `log_test` (18 cases), `persistence_test` (10 cases), `instance_manager_test`, `database_manager_test`: 20/20 each |
| ThreadSanitizer, `--output_base=/private/var/tmp/_bazel_persistence_tsan`, flags as in Phase 1, `--runs_per_test=10` | `frontend/persistence:{log_test,persistence_test}`, `transaction:concurrency_test`, `database:{database_concurrency_test,schema_lifetime_test}`, `locking:manager_test`, `common:clock_test`: 7 targets × 10 runs, no reports (the test binaries link `libclang_rt.tsan`) |
| Crash driver, persistent, `-model target -pushdown`, seeds 1–500, `-checkpoint-command 'kill -USR1 $EMULATOR_PID'` | 500 schedules, 15,508 steps, **0 mismatches**, 500 SIGKILL/restarts; 254 kills during a Commit: 128 interrupted RPCs, 44 successful replies, 82 matching terminal errors; 246 checkpoint requests |
| Target conformance, `--data_dir`, `-pushdown`, seeds 1–3000 | 3000 schedules, 49,332 steps, **0 mismatches**, `ABORTED` 607 / 607 (the same as without `--data_dir`) |
| Stress `crash` workload, 16 workers, 30 s, SIGKILL at 15 s | killed with 74,469 acknowledged transfers; restarted and verified exact in 0.6 s; exact again at the end, 143,646 transfers committed. Two more runs killed at 7 s and 23 s: exact too |
| Stelaxis smoke, `apps/stelaxis/priv/repo/structure.sql` | 158 statements (FKs last), 51 tables, 47 filled with 137 rows; after SIGKILL and restart the 136-statement DDL and every row are identical, and both sequence-keyed tables take new rows without reissuing a value |
| `lake build`; `grep -rn sorry`; Go vet, gofmt and `go test -race` under `verification/` | pass; no `sorry`; Lean files unchanged |

After the review fixes (the regression tests in `persistence_test` each
failed before their fix): the upstream suite again passes 132 of 134 targets
with the same two known macOS failures; `//frontend/persistence:all` and
`//common:clock_test` pass 20/20; ThreadSanitizer is clean on the same 7
targets × 10 runs; the crash driver passes seeds 1–500 with 0 mismatches
(129 interrupted commits, 43 successful replies, 246 checkpoint requests);
the stress `crash` workload is exact after SIGKILL (53,598 acknowledged at the
kill); the Stelaxis smoke test is identical after SIGKILL; `lake build` and
the Go checks pass.

After the second review (format version 2; each new regression test failed
before its fix): the suite passes 131 of 134 targets, the two known macOS
failures plus `session_manager_test`'s `CreateSession`, a race inherited from
#3 (it compares `absl::Now()` with the microsecond clock; 2 of 100 runs fail,
none of its code is touched here); the persistence, sequence, versioned
catalog and clock tests pass 20/20; ThreadSanitizer is clean on 7 targets × 10
runs; the crash driver passes seeds 1–500 with 0 mismatches (130 interrupted
commits, 246 checkpoint requests); the stress `crash` workload is exact after
SIGKILL (53,620 acknowledged; one in-flight transfer had committed); the
Stelaxis smoke test is identical after SIGKILL; `lake build` and the Go
checks pass.

After the third review (merged with #3 at 9d390401; each new regression test
failed before its fix): the suite passes 132 of 134 targets with only the two
known macOS failures; the persistence, storage, clock, versioned catalog and
session manager tests pass 20/20; ThreadSanitizer is clean on 7 targets × 10
runs; the crash driver passes seeds 1–500 with 0 mismatches (128 interrupted
commits, 246 checkpoint requests); the stress `crash` workload is exact
after SIGKILL (55,038 acknowledged; one in-flight transfer had committed);
the Stelaxis smoke test is identical after SIGKILL; `lake build` and the Go
checks pass.

Throughput, `stress -workers 16 -duration 15s`, native `emulator_main`,
committed transactions per second:

| Workload | In memory | `--data_dir` |
|---|---|---|
| contended | 1,148 | 1,331 |
| disjoint | 6,296 | 6,131 |
| mux282 | 15,125 | 11,669 |
| crash (32 accounts, SIGKILL mid-run) | | 4,784 |

On macOS `fsync` does not flush the drive's cache, so a sync costs little and
persistence costs little. On Linux, `fdatasync` bounds commits by the disk's
sync latency: every commit syncs its own record under the gate.
