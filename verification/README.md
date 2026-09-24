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
