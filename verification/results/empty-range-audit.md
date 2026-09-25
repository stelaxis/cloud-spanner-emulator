# Empty-range iteration audit

The input `(k,k)` normalizes to `[k.ToPrefixLimit(), k)`. If the
transaction has buffered writes, `lower_bound(start)` can be after
`lower_bound(limit)`, so an iterator incremented until it equals the latter
can run past the map's end.

## Product change

`backend/transaction/transaction_store.cc:253`: only enter the buffered-map
scan when `start < limit`. This also leaves `has_buffer` false for the later
merge loop. Base storage supplies the empty iterator. Lock acquisition and
pending-commit-timestamp validation still run.

## Other sites audited

- `backend/storage/in_memory_storage.cc:156,227`: read and delete already
  return before their `lower_bound` loops for `start >= limit`.
- `backend/transaction/read_write_transaction.cc:87`: `FlattenDeleteOp`
  reads through `TransactionStore::Read`, so the same fix makes empty delete
  ranges no-ops. Deleted-range bookkeeping at lines 529 and 555 iterates
  finite vectors; `Contains` rejects keys outside both bounds.
- `backend/datamodel/key_set.cc:76`: `MakeDisjointKeyRanges` normalizes
  ranges and merges a finite vector. It skips empty ranges after the first;
  an initial empty range can reach storage, which is why the guard belongs
  in `TransactionStore::Read` too. No endpoint-bounded map loop exists here.
- `backend/locking/manager.cc:46`: the current lock manager grants a single
  transaction-wide lock and does not iterate the requested range. Retaining
  lock acquisition preserves legacy contention behavior on empty reads.
- `backend/transaction/commit_timestamp.cc:155,182`: normalized-empty
  ranges are already skipped during delete validation and sentinel handling.
- `backend/datamodel/key_range.cc:126`: normalization preserves endpoint
  semantics; inverted normalized bounds are valid representations of an
  empty set and need not be rewritten.

## Regression coverage

- `backend/transaction/transaction_store_test.cc`: one parameterized test
  function, 18 collected cases (nine ranges, with/without buffered writes).
  Covers `(k,k)`, `[k,k)`, `(k,k]`, all four inverted endpoint combinations,
  a prefix range empty only after normalization, and the canonical empty
  range. Base storage is populated; buffered cases insert, update, and delete.
  A full read afterward verifies rows remain intact.
- `backend/transaction/read_write_transaction_test.cc`: one test function
  exercises empty-open, inverted, and normalized-equal delete ranges after
  buffered inserts, then commits and verifies all rows survive.
- `verification/conformance/empty_range_test.go`: explicitly expects empty
  results from both Lean models for the renamed crash regression and checks
  that the generator emits open-equal, inverted, and nonempty ranges.
- `verification/conformance/testdata/regression_empty_open_range.jsonl`:
  preserves the original five-step crash repro, adds inverted/equal-bound
  reads, and verifies the updated rows both before and after commit.

## Model

No Lean changes are needed. `verification/lean/TxnSpec/Basic.lean:36`
requires both endpoint predicates in `KeySpec.contains`, and
`verification/lean/TxnSpec/Local.lean:46` filters rows with that predicate.
`lake build` passes. There are no `sorry` or `native_decide` uses in the Lean
sources; the existing axiom audit reports only `propext`, `Classical.choice`,
and `Quot.sound` (or no axioms).

## Evidence

- `empty-range-bazel.txt`: fail-before on the unguarded source (5 of the
  18 store cases and the RW delete test throw on the walk past the map's end)
  and pass-after for `//backend/transaction/...`, `//backend/datamodel/...`
  and `//binaries:emulator_main`.
- `empty-range-conformance.txt`: both regression schedules and seeds 1–2000
  against `emulator_main` from this branch: 0 mismatches, with 2,601 empty
  or inverted range operations exercised.
- `empty-range-go-test.txt`, `empty-range-go-vet.txt`,
  `empty-range-lake-build.txt`: Go and Lean gates.
