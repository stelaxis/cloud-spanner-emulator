# L2 verification report — 2026-09-24

Branch: `lean-persistence-model`, stacked on `lean-txn-model` for integration
into [PR #1](https://github.com/stelaxis/cloud-spanner-emulator/pull/1).
No emulator implementation files changed. No separate L2 PR was opened.

## Crash/restart conformance

Image: `gcr.io/cloud-spanner-emulator/emulator:1.5.58`.
Digest: `sha256:c6f3402f2599684f295a0fdefb6fbbbfb18a0e43e309ff5456ccb452a4570a79`.
Each driver ran `emulator_main --abort_current_transaction_probability=0`
on its own ephemeral loopback port (none was 9010 or 9020), with its own
mounted data directory and container. All four completed with exit status 0,
and their owned containers were removed.

| Seed range | Steps | Mismatches | SIGKILL/restarts | Commit RPCs interrupted | Successful replies before kill |
|---|---:|---:|---:|---:|---:|
| [1–125](l2-no-persistence-1.txt) | 3,491 | 0 | 125 | 23 | 10 |
| [126–250](l2-no-persistence-126.txt) | 3,540 | 0 | 125 | 29 | 10 |
| [251–375](l2-no-persistence-251.txt) | 3,526 | 0 | 125 | 17 | 17 |
| [376–500](l2-no-persistence-376.txt) | 3,451 | 0 | 125 | 27 | 16 |
| **1–500** | **14,008** | **0** | **500** | **96** | **53** |

There were 254 Commit RPC crash attempts: 96 interrupted, 53 returning success
before SIGKILL, and 105 returning terminal transaction errors before the kill.
There were **0 checkpoint attempts**: upstream has no persistence/checkpoints.
The persistent-mode oracle and checkpoint-trigger hook are ready for Phase 2;
no live persistent-emulator conformance is claimed.

Exact gate commands, each from `verification/conformance` (file set: all Go
files in that module; executable `../lean/.lake/build/bin/txnmodel`):

```sh
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 125 -first-seed 1 -dump ../results/l2-failure-1.jsonl
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 125 -first-seed 126 -dump ../results/l2-failure-126.jsonl
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 125 -first-seed 251 -dump ../results/l2-failure-251.jsonl
mise exec go@1.25 -- go run . -persistence no-persistence -model upstream -seeds 125 -first-seed 376 -dump ../results/l2-failure-376.jsonl
```

No failure schedules were emitted by the completed runs. Earlier runs exposed
an empty crash-commit JSON array bug and too-short readiness polling after
restart; both were fixed before the successful reruns. The driver now waits
up to 60 seconds for the same restarted container and resets gRPC connection
backoff while it starts; it does not silently restart a second time or hide
a failed recovery.

Additional checks (same module/file set):

```sh
mise exec go@1.25 -- go run . -model upstream -seeds 500
mise exec go@1.25 -- go run -race . -persistence no-persistence -model upstream -seeds 10 -first-seed 11
mise exec go@1.25 -- go test -v ./...
mise exec go@1.25 -- go vet ./...
mise exec go@1.25 -- gofmt -l *.go
```

* [Original transaction harness](l2-legacy-upstream.txt): 500 schedules,
  8,167 steps, 0 mismatches; 1,130 ABORTED on each side.
* [Race-enabled crash run](l2-race.txt): 10 schedules, 305 steps, 10 SIGKILLs,
  3 interrupted commits, 0 mismatches and no Go race reports. These seeds
  overlap the 500-seed gate and are not counted as additional distinct seeds.
* [Go regressions](l2-go-tests.txt): 3 test functions, 11 leaf test cases
  (8 mode/model combinations, 1 torn-commit oracle check, 2 history/handle
  loss cases). All passed. The cases include an empty interrupted commit,
  both survival outcomes, two-table atomicity, repeated restarts, lost open
  transactions, and exact-history rejection including clamped future indices.
* `go vet ./...`: exit 0. `gofmt -l *.go`: no output. File set:
  `emulator.go`, `gen.go`, `main.go`, `model.go`, `restart.go`,
  `restart_test.go`, `schedule.go`.

## Lean gates and theorem map

Exact commands from the worktree root (file set: the entire
`verification/lean` Lake project, both `TxnSpec` and `txnmodel` targets):

```sh
lake -d verification/lean clean
lake -d verification/lean build
rg -n '\bsorry\b|\bnative_decide\b|^axiom\b' verification/lean --glob '*.lean'
```

[Clean build and axiom output](l2-lake-build.txt): success, no warnings or
admitted proofs. The source scan has no matches. All printed theorem
dependencies are subsets of `propext`, `Quot.sound`, `Classical.choice`.

| Contract | Proven theorem(s) |
|---|---|
| (a) | `Persistence.durability` |
| (b) | `Persistence.atomicity`, `recovery_prefix` (with the shared-WAL ordering fix below) |
| (c) | `Persistence.checkpoint_safety`, `quiescent_image` |
| (d) | `Persistence.every_post_restart_timestamp`, `checkpoint_clock_restart` |
| (e) | `Persistence.storage_sequences_never_reissue` (full storage/allocator product) |
| (f) | `Persistence.drop_record_survives`, `instance_drop_survives`, `dropped_stays_dropped` |
| (g) | `Persistence.target_serializable_across_restarts`, `replay_target_rows`, `open_transactions_lost` |

The machine's flush applies the pending resolved record to actual live data;
recovery reads only the published checkpoint and WAL tail. Ghost histories
are used to state/prove equivalence, not to implement either operation.

## Counterexamples and required design clarifications

* **Global timestamp prefix (b):** separate database mutexes permit reserve(1)
  on DB1, reserve/append/fsync(2) on DB2, then a crash. `[2]` is not a prefix
  of `[1,2]`; subsequent append(1) also breaks timestamp order. Checked by
  `per_database_locks_not_global_prefix`. The proved model adds a shared WAL
  gate around timestamp reservation/append; independent per-database WALs
  would instead need a per-database prefix statement.
* **Acknowledged-only interpretation of (g):** T1 fsyncs a write of 7 but its
  reply is lost; after recovery T2 reads 7 and commits. Omitting T1 is not
  serializable (`acknowledged_only_history_not_serializable`). Include durable
  completions of uncertain RPCs. Truly open/unpersisted transactions are lost.
* Checked bad variants also demonstrate pre-publication WAL deletion, cursor
  reuse after restart, wall-only clock restart, and rerunning backfills.
  The decided protocol already prevents these variants.

The [README](../README.md#l2-persistence-protocol-and-crash-conformance) details
abstractions and limits: resolved physical effects, opaque schema metadata,
fsync/atomic manifest publication assumptions, corruption detection boundary,
sequence incarnation IDs, no pre-restart MVCC service, and fixed-schema Target
composition. PostgreSQL, change streams, long-running-operation history and
backups remain out of scope.
