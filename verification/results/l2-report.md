# L2 round-2 review response — 2026-09-24

Branch: `lean-persistence-model`, stacked on `lean-txn-model` for integration
into [PR #1](https://github.com/stelaxis/cloud-spanner-emulator/pull/1).
This revision responds to the round-2 review of `4bbabb73`, following the
first review of `f38776c`. All changes are under `verification/`; there are no
emulator implementation changes or separate PR.

## Gates on the revised code

**Round 2 reruns Lean and Go gates only.** The Go driver and executable oracle
behavior are unchanged; the only Go edit documents the existing unsupported
single-use case. Per the task instruction, no emulator/Docker gate is rerun.

The retained no-persistence result was collected at `4bbabb73` using the existing image
`gcr.io/cloud-spanner-emulator/emulator:1.5.58`, digest
`sha256:c6f3402f2599684f295a0fdefb6fbbbfb18a0e43e309ff5456ccb452a4570a79`.
That run used one task-owned container at a time, capped with `--cpus=2`, and
built no Docker images. Each seed SIGKILLed and restarted that same
container/mount/port; only owned resources were removed. Unrelated development
containers were untouched.

**Retained result at `4bbabb73`: 500 distinct seeds (1–500), 15,508 steps,
0 mismatches, 500 SIGKILL/restarts; exit status 0.** Of the 254
crash-commit attempts, 116 were interrupted, 33 returned success before
SIGKILL, and 105 returned terminal errors that matched the model. Checkpoint
attempts: 0. The task-owned container was removed after completion.
[Resource-cap evidence](l2-review-resource-limits.txt) records two CPUs.
Raw log: [serial seeds 1–500](l2-review-no-persistence.txt).
The preceding [10-seed smoke run](l2-review-smoke.txt) passed (315 steps,
0 mismatches, 10 SIGKILL/restarts); its overlapping seeds are not counted again.
Historical four-shard/legacy logs belong to `f38776c`; neither set of Docker
logs is represented as a new round-2 run.

Recorded conformance command (not rerun), from `verification/conformance`:

```sh
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 500 -first-seed 1 -dump ../results/l2-review-failure.jsonl
```

Lean commands, **from `verification/lean`** so the pinned Lean 4.34.0 toolchain
is selected (file set: the entire Lake project, both `TxnSpec` and `txnmodel`):

```sh
lake clean && lake build
rg -n '\bsorry\b|\bnative_decide\b|^axiom\b' . --glob '*.lean'
```

[Round-2 build/axiom log](l2-round2-lake-build.txt): clean build passed with no warnings.
The source scan has no matches;
all audited theorem dependencies are subsets of `propext`, `Quot.sound`,
`Classical.choice`. There are no additional axioms or admitted proofs.

Go commands, from `verification/conformance`:

```sh
mise exec go@1.25 -- go test -race -count=1 ./...
mise exec go@1.25 -- go vet ./...
mise exec go@1.25 -- gofmt -l *.go
```

File set: `emulator.go`, `gen.go`, `main.go`, `model.go`, `restart.go`,
`restart_timestamps.go`, `restart_driver_test.go`, `restart_test.go`,
`schedule.go`. [Round-2 Go gate log](l2-round2-go-gates.txt). The unchanged suite has 5 test functions,
23 leaf cases (8 fake persistent-driver cases, 8 oracle mode/model cases,
4 terminal-error mode/model cases, 2 history/handle cases, 1 partial-oracle case).
The complete suite passed under the race detector. The 23 leaf cases are
listed in the [existing verbose test log](l2-review-go-tests.txt); round 2 uses
the exact non-verbose command above. `go vet ./...` exited 0 and
`gofmt -l *.go` produced no output. The fake driver tests exercise the actual
production gRPC/candidate code, with
only lifecycle commands substituted. No live persistent-emulator or implemented
checkpoint conformance is claimed; upstream has neither feature.

## Round-2 point-by-point response

### Blocking B3′ — exclude correct-protocol witnesses

The earlier three theorem statements could be satisfied during valid execution
of `Move`. Their replacements state actual invariant violations, and companion
lemmas prove the correct reachable machine excludes them:

| Faulty-machine witness | New violation | Correct-machine exclusion |
|---|---|---|
| `early_truncate_loses_recovery` | Running after restart, with an acknowledged boot record, but `disk.restore ≠ replay {} disk.log`. | `correct_recovery_excludes_early_truncate`, using `restore_eq`. |
| `wall_restart_regresses` | A correct reachable pre-restart state takes the faulty restart; its live epoch permits a timestamp at/below a previously served read. | `correct_restart_excludes_regression`, using `every_post_restart_timestamp`. |
| `rerun_backfill_changes_rows` | Running with an idle gate, but `visible ≠ replay {} disk.log` (concrete row values 8 versus 7). | `correct_quiescence_excludes_rerun`, using `quiescent_image`. |

The clock witness and its exclusion share `PostRestartRegression before start`:
there exist a state in the new live epoch, a timestamp above its reservation
clock, and a pre-restart served read at/above that timestamp. The witness pins
`start` to the result of the faulty wall-only restart of `before`; the exclusion
substitutes `restartState before wall`. Thus a commit at 3 followed later by a
read at 7 cannot satisfy this witness. A stopped process cannot satisfy the
recovery witness, and a durable-before-flush window cannot satisfy the idle
backfill witness. The three exclusion lemmas are included in `#print axioms`.
The other four counterexamples are unchanged.

### Non-blocking 1 — premature truncation fault

`BrokenMove.earlyTruncate` now requires an existing scratch snapshot and a
cutoff with `checkpoint.boundary < upto ≤ scratch.boundary`. It removes only
physical entries below that cutoff, exactly like the correct truncate operation
except for using an unpublished boundary. The trace captures a snapshot and
uses cutoff 1 while the published boundary is still 0; it no longer deletes
an arbitrary whole WAL without a precondition.

### Non-blocking 2 — product read scope

README explicitly states that product reads use ordinary Target calls, bypass
`serveRead`/the clock lease, and admit old exact ordinal reads after restart.
This more permissive serializability product does not prove the serving policy
of the storage clock model or executable restart oracle. After boot, its log
contains transaction records only: no DDL/catalog/sequence interleaving is
claimed in the Target composition (also addressing optional note 5).

### Non-blocking 3 — instrumented workload

README now says every commit receives a `RestartAudit` write and the first
commit also deletes all audit rows. The retained conformance numbers are for
that modified workload; they do not establish the uninstrumented L1 workload's
lock/conflict behavior.

### Non-blocking 4 — empty transaction-ID audit key

The wrapper comment and README now document that single-use commits are not
generated or supported: `GetTransactionId()` would be empty, causing such
commits to overwrite the same audit row. Adding support requires a distinct
key strategy. This follows the review's explicit option to document the
limitation; runtime behavior is unchanged, so no Docker rerun is needed.

## Earlier review responses (with corrected counterexample claims)

### B1 — storage/Target composition

`ProductMove` now pairs actual `Persistence.Move` transitions with Target.
`begin` stages the result of Target validation while holding the global gate;
only `flush` installs that result in the live Target. All Target calls require
an idle storage gate. Persisted-but-unflushed commits are tracked separately.
`ProductMove.crash` performs the actual storage crash and selects a durable
candidate from its storage phase, discarding open transaction handles.

* `product_target_inv`: every reachable product projects to
  `Persistence.Reachable`, `AcrossRestarts` and `Target.Inv`.
* `product_recovered_rows`: physical checkpoint/WAL recovery equals `stateAt`
  of the recovered Target history, including durable uncertain commits.
* `product_recovered_target_rows`: at idle states, including after crash/restart,
  the same equality holds for the ordinary Target projection.
* `product_visible_rows`: installed live rows equal Target at gate release.
* `product_timestamp_mapping`: ordinal timestamps `1..n`, positional mapping
  to physical records, and strict physical timestamp order.
* `target_serializable_across_restarts`: serializability follows for this product.

The README now says **one fixed-schema Target instance**, not each database.
Catalog/DDL physical safety does not imply SQL schema-validation concurrency.

### B2 — one physical WAL and the covered-replay hazard

`Disk.physical` stores sequence-numbered entries. Restore is precisely replay
of `checkpoint.image` followed by physical entries with `seq >= boundary`.
`wal` and `Snapshot.past` are ghost proof history, not recovery inputs.
Truncation can remove any prefix ending at/before the published boundary,
allowing partial deletions and crashes between them. Publication leaves the
physical WAL in place.

`physical_recovery` proves equality to both logical suffix replay and full
history replay for every reachable state. `next_sequence_physical` shows the
next append sequence is computable from the boundary and physical suffix count,
without stored ghost history. `covered_wal_replay_is_wrong` gives a reachable
published-checkpoint/crash trace under the faulty unfiltered-recovery transition:
the non-idempotent CREATE allocator effect runs twice. The correct filtered
restore yields allocator 1, while the faulty recovered live image has 2.

### B3 — real counterexamples and corrected claims

All seven listed faults below have reachable traces in faulty transition
systems, using normal protocol transitions plus the changed operation.
The three round-2 corrections now state violations of `restore_eq`,
`every_post_restart_timestamp` and `quiescent_image`, with companion
correct-machine exclusion lemmas listed above. `checkpointBytes` is documented as
stuttering because scratch is volatile and publication is atomic; partial
snapshot byte writes are not claimed to have been implemented or verified.
The tautological `recovery_prefix` was removed. The substantive claims now use
`log_monotone`, `durability`, `atomicity`, `restore_eq` and `physical_recovery`.

### N1 — exact gate and weaker alternatives

The proved shared gate runs from **begin through acknowledgement**; checkpoint
capture requires quiescence. Target reads and validation wait on that gate.
This is a sufficient global timestamp-ordering design, not a necessity claim:
per-database prefixes plus catalog-operation ordering could support a different
proof. Phase 2 should implement the stated global gate unless it proves a
weaker protocol.

### N2 — fsync matters

`Move.fsync` now takes `.complete` bytes and appends the durable physical record.
Optional early writeback uses `persist`, followed by `syncPersisted` when fsync
returns. `early_ack_loses_durability` runs begin/finish/early-ack/crash and exhibits
an acknowledged record missing from the durable log.

### N3 — limits of SIGKILL

README explicitly states that container SIGKILL preserves the kernel page cache
and cannot detect missing or misordered file/directory fsync. Phase 2 needs a
fault-injecting filesystem such as LazyFS and internal stage hooks. The Lean
proof assumes the durability/atomic-publication primitives in its transition
rules; it does not verify those filesystem implementations.

### N4 — terminal crash-commit results

The driver records terminal responses and sets `terminal` in each candidate. Supplied `durable`/`terminal` hints are
cleared first, so replay input cannot choose the observed outcome.
The executable model runs that commit even when `durable=false`, returning its
computed error for comparison. A fabricated ABORTED response for a duplicate
insert is rejected by the fake-emulator test. Direct oracle tests cover both
Target/Upstream and persistent/no-persistence modes.

### N5 — exact reads and hidden commit timestamps

Generated crash schedules perform an exact read before the crash and attempt
a saved pre-crash timestamp after restart. The additive `RestartAudit` table
stores a commit-timestamp column in the same RPC as user mutations. Reading it
after restart raises the floor even for ambiguous-but-durable commits. Served
read timestamps and successful replies also raise the floor. A fake lost-ack
commit followed by a regressed timestamp is rejected.

### N6 — persistent driver is executed

`TestPersistentCrashDriver` calls real `runCrashSchedule` against a fake gRPC
emulator. It verifies recovery polling through UNAVAILABLE/CREATING/READY,
whole-schedule candidate fan-out, successful-reply forced durability, correct
lost/full outcomes, rejection of partial A/B recovery, loss after success,
lost-ack timestamp regression, and wrong terminal errors. This establishes
harness behavior, not correctness of a future persistent emulator.

### N7 — executable oracle proof

`runRestartState` exposes the actual state fold used by `runRestartModel`.
`modelStep_target_across`, `runRestartState_target_across` and
`runRestartModel_target_inv` prove the Target projection of every schedule
prefix satisfies `AcrossRestarts` and `Target.Inv` (when keys-only read sets
are disabled). This is the executable oracle's invariant, distinct from the
storage-product refinement proved for B1.

### N8 — durable clock lease

`Disk.lease` is a durable upper bound; lease extensions precede serving read
or acknowledged commit timestamps beyond the previous bound. `serveRead` and
`ack` enforce the bound. `served_within_lease` and `acknowledged_within_lease`
prove it across transitions. Restart exceeds this lease even without a
checkpoint. `every_post_restart_timestamp` covers every commit reservation in
the new live epoch and proves it greater than every pre-crash served read,
recovered record timestamp, checkpoint clock and wall time.

### N9 — drop glue and definitional checks

`acknowledged_drop_recovered` takes reachability, an acknowledged DROP and no
later CREATE of the same incarnation in the durable log, and concludes that
physical recovery returns no database. It connects `durability`/`restore_eq`
to `drop_record_survives`. `corruption_fails` and `pre_restart_read_rejected`
remain policy-unfolding lemmas and are explicitly excluded from the substantive
theorem credit.

### N10 — monotone log

`log_monotone : Move s t → s.disk.log <+: t.disk.log` is proved for every
storage transition, including checkpoint publication, incremental truncation,
crash and restart. It replaces the former near-tautological prefix claim.

### N11 — torn-byte cleanup obligation

The `restartState` comment and README require physical truncation of torn
final bytes before the next append. Clearing the abstract flag is the model
of that obligation, not permission to append after torn bytes.

### Non-vacuity suggestion

Kernel-checked `example`s exhibit a nonempty acknowledged history, published
checkpoint boundary above zero, crash/restart, and a nonempty Target product
history surviving its actual storage crash.

## Theorems (a)–(g)

| Contract | Proofs on the corrected protocol |
|---|---|
| (a) | `durability`, `restore_eq` |
| (b) | `atomicity`, `log_monotone`, `physical_recovery` |
| (c) | `checkpoint_safety`, `physical_recovery`, `quiescent_image` |
| (d) | `every_post_restart_timestamp`, `served_within_lease`, `acknowledged_within_lease` |
| (e) | `storage_sequences_never_reissue`, `allocator_refines` |
| (f) | `acknowledged_drop_recovered`, `drop_record_survives`, `instance_drop_survives` |
| (g) | `product_target_inv`, `product_recovered_rows`, `product_recovered_target_rows`, `product_timestamp_mapping`, `target_serializable_across_restarts` |

All persistence theorem names above are in `TxnSpec.Persistence`.
Oracle theorems are in `TxnSpec.Json`.

## Counterexamples and fixes

| Reachable faulty transition trace | Theorem | Fix |
|---|---|---|
| Acknowledge complete, unpersisted bytes; crash | `early_ack_loses_durability` | Fsync/install before ack. |
| Capture, truncate to unpublished boundary, crash/restart; running recovery differs from logical replay | `early_truncate_loses_recovery` refutes `restore_eq`/`physical_recovery` | Truncate only below published boundary. |
| Publish; crash; replay covered WAL atop image | `covered_wal_replay_is_wrong` | Sequence-filter physical WAL. |
| Lease 10; read 7; crash; wall-only restarted epoch permits reservation 3 ≤ 7 | `wall_restart_regresses` refutes `every_post_restart_timestamp` | Durable lease/recovered-clock restart floor. |
| Reserve; issue 0; reset cursor; issue 0 again | `cursor_reset_reissues` | Abandon range; restart at durable upper bound. |
| Backfill result 7; crash; replay then re-run SQL; live idle image is 8 rather than 7 | `rerun_backfill_changes_rows` refutes `quiescent_image` | Resolved physical replay, never re-execute. |
| DB0 reserves 2; DB1 appends 3; DB0 appends 2 | `per_database_gate_reorders_wal` | Shared gate for the global-order strengthening. |
