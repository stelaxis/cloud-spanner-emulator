/-!
# Shared vocabulary

A table has an INT64 key column `K` and an INT64 value column `V` that is never
written as NULL. A row either exists with a value or is absent, so a *view* of
the database is a function `RowKey → Option Val`.

Keys are drawn from a bounded universe (`keys`) so that range reads are
computable; the conformance harness only uses keys inside it.
-/

namespace TxnSpec

abbrev Key := Nat
abbrev Val := Int
abbrev Tid := Nat

structure RowKey where
  tbl : Nat
  key : Key
deriving DecidableEq, Repr

/-- Keys are `0 … keyBound-1`. -/
def keyBound : Nat := 16

def keys : List Key := List.range keyBound

/-- A key set over one table: a point, an interval with open/closed ends, or the
whole table. -/
inductive KeySpec where
  | point (k : Key)
  | range (lo : Key) (loIncl : Bool) (hi : Key) (hiIncl : Bool)
  | all
deriving DecidableEq, Repr

def KeySpec.contains : KeySpec → Key → Bool
  | .point k, x => x == k
  | .range lo li hi hc, x =>
      (if li then decide (lo ≤ x) else decide (lo < x)) &&
      (if hc then decide (x ≤ hi) else decide (x < hi))
  | .all, _ => true

/-- A read-set / footprint element: a key set in one table. Columns are not
tracked (see the README: row granularity). -/
structure Span where
  tbl : Nat
  spec : KeySpec
deriving DecidableEq, Repr

def Span.covers (s : Span) (rk : RowKey) : Bool :=
  s.tbl == rk.tbl && s.spec.contains rk.key

def covered (S : List Span) (rk : RowKey) : Bool :=
  S.any fun s => s.covers rk

abbrev View := RowKey → Option Val

def emptyView : View := fun _ => none

/-- Two views agree on every row covered by `S`. -/
def Agree (V₁ V₂ : View) (S : List Span) : Prop :=
  ∀ rk, covered S rk = true → V₁ rk = V₂ rk

/-! ## Write lists

A write list maps row keys to their new state (`none` = tombstone). The head
entry wins, so later writes are consed on. The same shape serves as a
transaction's buffer and as a commit's effect on the MVCC store. -/

abbrev Writes := List (RowKey × Option Val)

def lookupW : Writes → RowKey → Option (Option Val)
  | [], _ => none
  | (k, v) :: rest, rk => if k = rk then some v else lookupW rest rk

/-- `ws` laid over `V`. -/
def overlay (ws : Writes) (V : View) : View :=
  fun rk => (lookupW ws rk).getD (V rk)

/-! ## Operations and results -/

inductive MutKind where
  | insert | update | upsert | delete
deriving DecidableEq, Repr

/-- A mutation sent with `Commit`. `val` is ignored for `delete`. -/
structure Mut where
  kind : MutKind
  tbl : Nat
  key : Key
  val : Val
deriving DecidableEq, Repr

def Mut.rk (m : Mut) : RowKey := ⟨m.tbl, m.key⟩

inductive Op where
  | beginRW
  /-- `none` = strong; `some n` = exact timestamp of the `n`-th commit
  (clamped to the commits so far). -/
  | beginRO (at? : Option Nat)
  /-- `Read` RPC with one key or one range. -/
  | read (tbl : Nat) (spec : KeySpec)
  /-- `SELECT K, V FROM t WHERE <spec> ORDER BY K`. -/
  | sql (tbl : Nat) (spec : KeySpec)
  | dmlInsert (tbl : Nat) (key : Key) (val : Val)
  | dmlUpdate (tbl : Nat) (spec : KeySpec) (val : Val)
  | dmlDelete (tbl : Nat) (spec : KeySpec)
  | commit (muts : List Mut)
  | rollback
deriving DecidableEq, Repr

/-- The data operations a RW transaction executes before `Commit`. -/
def Op.isData : Op → Bool
  | .read .. | .sql .. | .dmlInsert .. | .dmlUpdate .. | .dmlDelete .. => true
  | _ => false

/-- gRPC status codes the model can produce. -/
inductive Code where
  | aborted | alreadyExists | notFound | invalidArgument | failedPrecondition
deriving DecidableEq, Repr

inductive Res where
  | ok
  | rows (rs : List (Key × Val))
  | count (n : Nat)
  | committed (ts : Nat)
  | err (c : Code)
deriving DecidableEq, Repr

structure Step where
  txn : Tid
  op : Op
deriving DecidableEq, Repr

/-- Pointwise function update. -/
def upd {α : Type} (f : Tid → α) (t : Tid) (x : α) : Tid → α :=
  fun t' => if t' = t then x else f t'

@[simp] theorem upd_same {α : Type} (f : Tid → α) (t : Tid) (x : α) : upd f t x t = x := by
  simp [upd]

@[simp] theorem upd_other {α : Type} (f : Tid → α) (t t' : Tid) (x : α) (h : t' ≠ t) :
    upd f t x t' = f t' := by
  simp [upd, h]

end TxnSpec
