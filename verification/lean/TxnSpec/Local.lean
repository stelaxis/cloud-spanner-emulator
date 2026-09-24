import TxnSpec.Basic

/-!
# Transaction-local semantics

What one read-write transaction's operation returns and buffers, given the
*base view* it reads from. Both models reuse this: `Upstream` passes the latest
committed state, `Target` the state at the transaction's snapshot.

Mirrors the emulator's `TransactionStore` overlay (reads see the transaction's
own buffered writes) and the per-transaction deleted-key list that makes an
`update` mutation of a key the transaction deleted fail with
`INVALID_ARGUMENT` (`ReadWriteTransaction::Write`).
-/

namespace TxnSpec

structure Local where
  buf : Writes := []
  /-- Keys deleted by this transaction and not re-inserted since. -/
  deleted : List RowKey := []
deriving DecidableEq, Repr

def Local.view (l : Local) (V : View) : View := overlay l.buf V

def Local.put (l : Local) (rk : RowKey) (v : Val) : Local :=
  { l with buf := (rk, some v) :: l.buf, deleted := l.deleted.erase rk }

/-- Delete of a visible row: tombstone plus the deleted-key record. -/
def Local.del (l : Local) (rk : RowKey) : Local :=
  { buf := (rk, none) :: l.buf, deleted := rk :: l.deleted }

/-- Delete of an invisible row buffers nothing (the emulator's `FlattenDeleteOp`
yields no write op) but still records the key as deleted. -/
def Local.markDeleted (l : Local) (rk : RowKey) : Local :=
  { l with deleted := rk :: l.deleted }

/-- Rows of `tbl` matching `spec`, in key order. -/
def rowsOf (V : View) (tbl : Nat) (spec : KeySpec) : List (Key × Val) :=
  keys.filterMap fun k => if spec.contains k then (V ⟨tbl, k⟩).map fun v => (k, v) else none

/-- Existing keys of `tbl` matching `spec`, in key order. -/
def matched (V : View) (tbl : Nat) (spec : KeySpec) : List Key :=
  keys.filter fun k => spec.contains k && (V ⟨tbl, k⟩).isSome

def applyMut (V : View) (l : Local) (m : Mut) : Except Code Local :=
  let W := l.view V
  match m.kind with
  | .insert => if (W m.rk).isSome then .error .alreadyExists else .ok (l.put m.rk m.val)
  | .update =>
      if m.rk ∈ l.deleted then .error .invalidArgument
      else if (W m.rk).isNone then .error .notFound
      else .ok (l.put m.rk m.val)
  | .upsert => .ok (l.put m.rk m.val)
  | .delete => if (W m.rk).isSome then .ok (l.del m.rk) else .ok (l.markDeleted m.rk)

def applyMuts (V : View) : Local → List Mut → Except Code Local
  | l, [] => .ok l
  | l, m :: ms => match applyMut V l m with
    | .ok l' => applyMuts V l' ms
    | .error c => .error c

def putAll (tbl : Nat) (v : Val) : Local → List Key → Local
  | l, [] => l
  | l, k :: ks => putAll tbl v (l.put ⟨tbl, k⟩ v) ks

def delAll (tbl : Nat) : Local → List Key → Local
  | l, [] => l
  | l, k :: ks => delAll tbl (l.del ⟨tbl, k⟩) ks

/-- One data operation of a read-write transaction against base view `V`. -/
def Local.step (V : View) (l : Local) : Op → Except Code (Res × Local)
  | .read tbl spec => .ok (.rows (rowsOf (l.view V) tbl spec), l)
  | .sql tbl spec => .ok (.rows (rowsOf (l.view V) tbl spec), l)
  | .dmlInsert tbl k v => match applyMut V l ⟨.insert, tbl, k, v⟩ with
    | .ok l' => .ok (.count 1, l')
    | .error c => .error c
  | .dmlUpdate tbl spec v =>
      let ks := matched (l.view V) tbl spec
      .ok (.count ks.length, putAll tbl v l ks)
  | .dmlDelete tbl spec =>
      let ks := matched (l.view V) tbl spec
      .ok (.count ks.length, delAll tbl l ks)
  | _ => .error .failedPrecondition

/-- Run a transaction's data operations in order from an empty buffer. -/
def runProg (V : View) : Local → List Op → Except Code (List Res × Local)
  | l, [] => .ok ([], l)
  | l, op :: ops => match Local.step V l op with
    | .ok (r, l') => match runProg V l' ops with
      | .ok (rs, l'') => .ok (r :: rs, l'')
      | .error c => .error c
    | .error c => .error c

/-- A whole transaction: data operations, then the commit's mutations. -/
def runTxn (V : View) (prog : List Op) (muts : List Mut) : Except Code (List Res × Local) :=
  match runProg V {} prog with
  | .ok (rs, l) => match applyMuts V l muts with
    | .ok l' => .ok (rs, l')
    | .error c => .error c
  | .error c => .error c

/-! ## Footprints

The rows whose base value an operation may depend on. `pushdown = false` is the
emulator today: every SQL statement scans the whole table
(`queryable_table.cc`, `KeySet::All`). -/

structure Cfg where
  /-- SQL predicates narrow the read set to the predicate's key set. -/
  pushdown : Bool := false
  /-- Take the snapshot at `BeginTransaction` instead of at the first data
  operation (design variant; breaks `target_admits_upstream`). -/
  snapAtBegin : Bool := false
  /-- On a constraint error, validate the read set first and report `ABORTED`
  if it is stale (design fix; see `target_errors_serial`). -/
  validateErrors : Bool := true
  /-- Record only the rows a read returned instead of the key set it scanned
  (design variant; admits phantoms). -/
  keysOnlyReadSet : Bool := false
deriving DecidableEq, Repr

def sqlSpan (cfg : Cfg) (tbl : Nat) (spec : KeySpec) : Span :=
  ⟨tbl, if cfg.pushdown then spec else .all⟩

def Mut.span (m : Mut) : Span := ⟨m.tbl, .point m.key⟩

def fp (cfg : Cfg) : Op → List Span
  | .read tbl spec => [⟨tbl, spec⟩]
  | .sql tbl spec => [sqlSpan cfg tbl spec]
  | .dmlInsert tbl k _ => [⟨tbl, .point k⟩]
  | .dmlUpdate tbl spec _ => [sqlSpan cfg tbl spec]
  | .dmlDelete tbl spec => [sqlSpan cfg tbl spec]
  | .commit ms => ms.map Mut.span
  | _ => []

/-- Rows an operation may write (data-independent over-approximation). -/
def writeSpans : Op → List Span
  | .dmlInsert tbl k _ => [⟨tbl, .point k⟩]
  | .dmlUpdate tbl spec _ => [⟨tbl, spec⟩]
  | .dmlDelete tbl spec => [⟨tbl, spec⟩]
  | .commit ms => ms.map Mut.span
  | _ => []

end TxnSpec
