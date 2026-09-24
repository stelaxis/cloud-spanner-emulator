import TxnSpec.Serial
import TxnSpec.Upstream
import TxnSpec.Target

/-!
# Counterexamples: design variants that break a property

Each is a concrete schedule checked by `decide` (kernel evaluation). Tables:
`0` = `A`, `1` = `B`.
-/

namespace TxnSpec.Counterexamples

open TxnSpec

/-- A decidable form of `Serializable`, for concrete logs. -/
def validB (V : View) (c : Commit) : Bool :=
  match runTxn V c.prog c.muts with
  | .ok (rs, l) => rs == c.results && l == c.final
  | .error _ => false

def serialB : View → List Commit → Bool
  | _, [] => true
  | V, c :: cs => validB V c && serialB (overlay c.writes V) cs

theorem validB_iff (V : View) (c : Commit) : validB V c = true ↔ c.Valid V := by
  unfold validB Commit.Valid
  split <;> simp_all

theorem serialB_iff (V : View) (cs : List Commit) : serialB V cs = true ↔ SerialFrom V cs := by
  induction cs generalizing V with
  | nil => simp [serialB, SerialFrom]
  | cons c cs ih => simp [serialB, SerialFrom, validB_iff, ih]

def setup (rows : List (Nat × Key × Val)) : List Step :=
  [⟨0, .beginRW⟩, ⟨0, .commit (rows.map fun (t, k, v) => ⟨.insert, t, k, v⟩)⟩]

/-! ## Phantoms: the read set must record key *ranges*, not rows returned

T1 scans `A[0,3]` and sees one row. T2 inserts `A{2}` and commits. T1 then
commits. Recording only the returned row `A{1}`, validation misses T2's insert;
serially (T2 before T1) T1's scan would have returned two rows. -/

def phantom : List Step :=
  setup [(0, 1, 10)] ++
  [⟨1, .beginRW⟩, ⟨1, .sql 0 (.range 0 true 3 true)⟩,
   ⟨2, .beginRW⟩, ⟨2, .dmlInsert 0 2 20⟩, ⟨2, .commit []⟩,
   ⟨1, .dmlInsert 1 0 1⟩, ⟨1, .commit []⟩]

example : (Target.run { keysOnlyReadSet := true } {} phantom).1.getLast? = some (.committed 3) := by
  decide

theorem keysOnly_not_serializable :
    ¬ Serializable (Target.run { keysOnlyReadSet := true } {} phantom).2.log := by
  rw [Serializable, ← serialB_iff]; decide

/-- The design as specified (key-range read sets) aborts T1 instead. -/
example : (Target.run {} {} phantom).1.getLast? = some (.err .aborted) := by decide

/-! ## Snapshot at `BeginTransaction` rejects histories Upstream commits

T1 begins, T2 updates `A{1}` and commits, then T1 reads `A{1}` and commits.
Upstream commits both (T1 takes the lock only at its read). With the snapshot
taken at begin, T1 reads the stale `A{1}` and fails validation. Taking the
snapshot at the first data operation (the model's default) fixes it; see
`target_admits_upstream`. -/

def lateReader : List Step :=
  setup [(0, 1, 10)] ++
  [⟨1, .beginRW⟩,
   ⟨2, .beginRW⟩, ⟨2, .dmlUpdate 0 (.point 1) 11⟩, ⟨2, .commit []⟩,
   ⟨1, .read 0 (.point 1)⟩, ⟨1, .commit []⟩]

example : (Upstream.run {} lateReader).1.getLast? = some (.committed 3) := by decide
example : (Target.run {} {} lateReader).1.getLast? = some (.committed 3) := by decide

theorem snapAtBegin_rejects :
    (Target.run { snapAtBegin := true } {} lateReader).1.getLast? = some (.err .aborted) := by
  decide

/-! ## Constraint errors from a stale snapshot must become `ABORTED`

T1 reads `B{0}` (fixing its snapshot while `A{1}` exists). T2 deletes `A{1}`
and commits. T1 inserts `A{1}`: against its snapshot the row exists, so a naive
implementation returns `ALREADY_EXISTS` — an error no serial order produces
(serially, after T2, the insert succeeds), and one clients do not retry. The
model validates first and returns `ABORTED`; see `target_errors_serial`. -/

def staleConstraint : List Step :=
  setup [(0, 1, 10)] ++
  [⟨1, .beginRW⟩, ⟨1, .read 1 (.point 0)⟩,
   ⟨2, .beginRW⟩, ⟨2, .dmlDelete 0 (.point 1)⟩, ⟨2, .commit []⟩,
   ⟨1, .dmlInsert 0 1 7⟩]

theorem naive_errors_not_serial :
    (Target.run { validateErrors := false } {} staleConstraint).1.getLast? =
      some (.err .alreadyExists) := by
  decide

example : (Target.run {} {} staleConstraint).1.getLast? = some (.err .aborted) := by decide

/-! ## Delete-then-reinsert (ABA) is caught

T2 deletes `A{1}`, T3 re-inserts it with the same value; T1 read `A{1}` before
both. Validation works on versions, not values, so T1 aborts although the value
it read is back. Serializability does not need this abort (the value is
unchanged), but reporting it is safe. -/

def aba : List Step :=
  setup [(0, 1, 10)] ++
  [⟨1, .beginRW⟩, ⟨1, .read 0 (.point 1)⟩,
   ⟨2, .beginRW⟩, ⟨2, .commit [⟨.delete, 0, 1, 0⟩]⟩,
   ⟨3, .beginRW⟩, ⟨3, .commit [⟨.insert, 0, 1, 10⟩]⟩,
   ⟨1, .dmlInsert 1 0 1⟩, ⟨1, .commit []⟩]

example : (Target.run {} {} aba).1.getLast? = some (.err .aborted) := by decide

/-! ## Blind writes do not conflict

Two transactions upsert the same row without reading anything else. Mutations
record an existence read of their row (the emulator's flattening looks it up),
so the first committer invalidates the second: last-writer-wins would be
serializable too, but the model follows the emulator's read. -/

def blind : List Step :=
  setup [] ++
  [⟨1, .beginRW⟩, ⟨2, .beginRW⟩,
   ⟨1, .read 1 (.point 5)⟩, ⟨2, .read 1 (.point 6)⟩,
   ⟨1, .commit [⟨.upsert, 0, 1, 1⟩]⟩, ⟨2, .commit [⟨.upsert, 0, 1, 2⟩]⟩]

example : (Target.run {} {} blind).1.drop 6 = [.committed 2, .err .aborted] := by decide

/-! ## Schedule-for-schedule, neither model is more permissive

On the same schedule Upstream commits T1 and aborts T2 (T2 touched the slot),
while Target lets T2 commit and then aborts T1 (T2 wrote what T1 read). This is
why `target_admits_upstream` compares Target on the *committed* transactions'
steps. -/

def contended : List Step :=
  setup [(0, 1, 10)] ++
  [⟨1, .beginRW⟩, ⟨1, .read 0 (.point 1)⟩,
   ⟨2, .beginRW⟩, ⟨2, .dmlUpdate 0 (.point 1) 11⟩, ⟨2, .commit []⟩,
   ⟨1, .dmlInsert 1 0 1⟩, ⟨1, .commit []⟩]

example : (Upstream.run {} contended).1.drop 5 =
    [.err .aborted, .err .aborted, .count 1, .committed 2] := by decide
example : (Target.run {} {} contended).1.drop 5 =
    [.count 1, .committed 2, .count 1, .err .aborted] := by decide

theorem target_differs_unfiltered :
    (Upstream.run {} contended).2.log.map (·.tid) = [0, 1] ∧
      (Target.run {} {} contended).2.log.map (·.tid) = [0, 2] := by
  decide

end TxnSpec.Counterexamples
