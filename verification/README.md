# Transaction model: proofs and conformance

Before changing how the emulator runs read-write transactions, this directory
pins down two things:

* **Upstream**: what the emulator does today (v1.5.58, one lock slot per
  database). A Lean 4 model, checked step by step against the real emulator.
* **Target**: the optimistic design meant to replace it. A Lean 4 model with
  proofs that it is serializable, that transactions touching disjoint data
  don't abort each other, and that it accepts every history Upstream commits.

The conformance harness runs the same random schedules through the emulator
and a model and diffs every step. Today it must match **Upstream**. Once the
emulator implements the new design, it must match **Target**.

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

The gateway does not pass `--abort_current_transaction_probability` through
(`binaries/gateway_main.go:52-71`), so the harness runs `emulator_main`
directly:

```sh
cd verification/conformance
(cd ../lean && lake build)                           # builds txnmodel
mise exec go@1.25 -- go run . -seeds 2000            # starts the 1.5.58 image itself
# or against a running emulator_main started with --abort_current_transaction_probability=0:
SPANNER_EMULATOR_HOST=localhost:9010 mise exec go@1.25 -- go run . -seeds 2000
```

Useful flags:

| Flag | Effect |
|---|---|
| `-model upstream` (default) | Upstream must match; exit 1 on any mismatch. |
| `-model target [-pushdown] -must-match=false` | Report how the emulator diverges from Target. Phase 1 drops `-must-match=false`. |
| `-image` | The emulator image to start. |
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
proceeds. That is the Phase 1 gap: the emulator aborts transactions because of
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
