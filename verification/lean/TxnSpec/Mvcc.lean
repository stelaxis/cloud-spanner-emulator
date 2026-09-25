import TxnSpec.Local

/-!
# MVCC store and per-transaction state

The store is the list of committed transactions in commit order; the `i`-th
entry (0-based) carries commit timestamp `i + 1`, and every row version it
wrote is stamped with that timestamp. `stateAt log n` is the snapshot at
timestamp `n`: the effect of exactly the commits with `ts ≤ n`, i.e. the
emulator's `Storage::Read(n, …)` over `(key → column → time → value)` with
tombstones. Timestamp `0` is the empty database.

Logical timestamps stand in for the emulator's microsecond clock, which is
strictly increasing (`Clock::Now`, `common/clock.cc`).
-/

namespace TxnSpec

/-- A committed transaction: its program and what it observed. -/
structure Commit where
  tid : Tid
  ts : Nat
  prog : List Op
  results : List Res
  muts : List Mut
  final : Local
deriving DecidableEq, Repr

def Commit.writes (c : Commit) : Writes := c.final.buf

def applyLog (V : View) : List Commit → View
  | [] => V
  | c :: cs => applyLog (overlay c.writes V) cs

def stateAt (log : List Commit) (n : Nat) : View :=
  applyLog emptyView (log.take n)

/-- Does `c` write a row covered by `S`? -/
def conflicts (S : List Span) (c : Commit) : Bool :=
  c.writes.any fun w => covered S w.1

/-! ## Transactions -/

inductive Kind where
  | rw | ro
deriving DecidableEq, Repr

inductive Status where
  | idle
  | active
  /-- Failed with `c`; every later call except `Rollback` replays `c`
  (frontend `Transaction::GuardedCall`). -/
  | dead (c : Code)
  | committed (ts : Nat)
  | rolledBack
deriving DecidableEq, Repr

structure TxnSt where
  kind : Kind := .rw
  status : Status := .idle
  /-- Snapshot timestamp: RO read timestamp; Target RW start timestamp. -/
  snap : Option Nat := none
  loc : Local := {}
  /-- Target read set. -/
  rset : List Span := []
  /-- Successful data operations and their results, in order. -/
  prog : List Op := []
  results : List Res := []
deriving Repr

/-- What an RO transaction's read returns at snapshot `V`. -/
def roRead (V : View) : Op → Option Res
  | .read tbl spec => some (.rows (rowsOf V tbl spec))
  | .sql tbl spec => some (.rows (rowsOf V tbl spec))
  | _ => none

/-- Snapshot index for `BeginTransaction(read_only)`. -/
def roSnap (logLen : Nat) : Option Nat → Nat
  | none => logLen
  | some n => min n logLen

/-- Calls on a transaction that is not `active`; shared by both models.
Returns the result and the new transaction state. -/
def inactiveStep (x : TxnSt) (op : Op) : Res × TxnSt :=
  match x.status, op with
  | .dead _, .rollback => (.ok, { x with status := .rolledBack })
  | .dead c, _ => (.err c, x)
  | .committed ts, .commit _ => (.committed ts, x)
  | .rolledBack, .rollback => (.ok, x)
  | _, _ => (.err .failedPrecondition, x)

/-- Begin on an `idle` transaction. -/
def beginStep (logLen : Nat) (x : TxnSt) : Op → Res × TxnSt
  | .beginRW => (.ok, { x with status := .active, kind := .rw })
  | .beginRO at? => (.ok, { x with status := .active, kind := .ro, snap := some (roSnap logLen at?) })
  | _ => (.err .failedPrecondition, x)

/-- Any call on an active RO transaction. -/
def roStep (log : List Commit) (x : TxnSt) (op : Op) : Res × TxnSt :=
  match roRead (stateAt log (x.snap.getD 0)) op with
  | some r => (r, { x with prog := x.prog ++ [op], results := x.results ++ [r] })
  | none => (.err .failedPrecondition, x)

end TxnSpec
