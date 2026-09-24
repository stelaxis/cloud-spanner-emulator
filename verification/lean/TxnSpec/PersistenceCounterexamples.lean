import TxnSpec.PersistenceComposition
import TxnSpec.PersistenceSequences

namespace TxnSpec.Persistence

/-- Each variant changes an actual transition, keeping all other Move steps. -/
inductive Fault where
  | earlyAck | earlyTruncate | wallClock | rerunBackfill | ignoreBoundary | perDatabaseGate

def crashed (s : State) : State :=
  { s with pending := .idle, visible := {}, scratch := none, running := false, clock := 0, restartBoundary := 0 }

def incrementBackfill (s : Image) : Image :=
  { s with databases := upd s.databases 0 ((s.databases 0).map fun b =>
    { b with rows := overlay [(⟨0, 0⟩, some ((b.rows ⟨0, 0⟩).getD 0 + 1))] b.rows }) }

inductive BrokenMove : Fault → State → State → Prop where
  | good {f : Fault} {s t : State} : Move s t → BrokenMove f s t
  | earlyAck (s : State) (r : Record) (hp : s.pending = .complete r)
      (hl : r.ts ≤ s.disk.lease) :
      BrokenMove .earlyAck s { s with acks := s.acks ++ [r] }
  | earlyTruncate (s : State) :
      BrokenMove .earlyTruncate s { s with disk := { s.disk with physical := [] } }
  | wallRestart (s : State) (wall : Nat) (hr : s.running = false) :
      BrokenMove .wallClock s { restartState s wall with clock := wall + 1, restartBoundary := wall + 1 }
  | rerun (s : State) (wall : Nat) (hr : s.running = false) :
      BrokenMove .rerunBackfill s { restartState s wall with visible := incrementBackfill s.disk.restore }
  | unfiltered (s : State) (wall : Nat) (hr : s.running = false) :
      BrokenMove .ignoreBoundary s { restartState s wall with visible := replay s.disk.checkpoint.image (s.disk.physical.map WALEntry.record) }
  /-- Another database can append while the first database holds its own gate.
  Its later timestamp reaches the shared WAL before the reserved earlier one. -/
  | foreignAppend (s : State) (r : Record) :
      BrokenMove .perDatabaseGate s { s with disk := { s.disk with wal := s.disk.wal ++ [r], physical := s.disk.physical ++ [⟨s.disk.log.length, r⟩], lease := max s.disk.lease r.ts }, visible := applyRecord s.visible r, clock := max s.clock r.ts, acks := s.acks ++ [r] }

inductive BrokenReachable (f : Fault) : State → Prop where
  | initial : BrokenReachable f {}
  | next {s t : State} : BrokenReachable f s → BrokenMove f s t → BrokenReachable f t

theorem lift_reachable {f : Fault} {s : State} (h : Reachable s) : BrokenReachable f s := by
  induction h with
  | initial => exact .initial
  | next _ m ih => exact .next ih (.good m)

theorem broken_write {f : Fault} {s : State} (hs : BrokenReachable f s) (r : Record)
    (hi : s.pending = .idle) (hr : s.running = true) (hc : s.clock < r.ts) :
    BrokenReachable f (writeRecord s r) := by
  have h₀ := BrokenReachable.next hs (.good (.lease s (max s.disk.lease r.ts) (Nat.le_max_left _ _)))
  have h₁ := BrokenReachable.next h₀ (.good (.begin _ r hi hr hc))
  have h₂ := BrokenReachable.next h₁ (.good (.finish _ r rfl))
  have h₃ := BrokenReachable.next h₂ (.good (.fsync _ r rfl))
  have h₄ := BrokenReachable.next h₃ (.good (.flush _ r rfl))
  have h₅ := BrokenReachable.next h₄ (.good (.ack _ r rfl (Nat.le_max_right _ _)))
  simpa only [writeRecord, hi, Disk.log] using h₅

/-- Refutes (a): begin → finish → acknowledge complete bytes → crash.
The corrected fsync transition appends to durable physical WAL before ack. -/
theorem early_ack_loses_durability : ∃ s, BrokenReachable .earlyAck s ∧
    bootRecord ∈ s.acks ∧ bootRecord ∉ s.disk.log := by
  have h₀ := BrokenReachable.next (.initial (f := .earlyAck)) (.good (.lease {} 1 (by decide)))
  have h₁ := BrokenReachable.next h₀
    (.good (.begin _ bootRecord rfl rfl (by decide)))
  have h₂ := BrokenReachable.next h₁ (.good (.finish _ bootRecord rfl))
  have h₃ := BrokenReachable.next h₂ (.earlyAck _ bootRecord rfl (by decide))
  have h₄ := BrokenReachable.next h₃ (.good (.crash _))
  exact ⟨_, h₄, by simp, by simp [Disk.log]⟩

/-- Refutes (c): acknowledged WAL is truncated before any manifest publication;
crash/restart recovers no effect although the acknowledged record is in history. -/
theorem early_truncate_loses_recovery : ∃ s, BrokenReachable .earlyTruncate s ∧
    bootRecord ∈ s.acks ∧ s.visible.databases 0 = none := by
  have h₁ := lift_reachable (f := .earlyTruncate) (writeRecord_reachable .initial bootRecord rfl rfl (by decide))
  have hs := BrokenReachable.next h₁ (.good (.checkpointStart _ rfl rfl))
  have h₂ := BrokenReachable.next hs (.earlyTruncate _)
  have h₃ := BrokenReachable.next h₂ (.good (.crash _))
  have h₄ := BrokenReachable.next h₃ (.good (.restart _ 0 rfl rfl rfl))
  exact ⟨_, h₄, by simp [writeRecord, restartState], rfl⟩

/-- Refutes (d) including read observations: lease 10 → serve read at 7 →
crash → wall-only restart at 2 → commit at 3. -/
theorem wall_restart_regresses : ∃ s, BrokenReachable .wallClock s ∧
    7 ∈ s.served ∧ ∃ r ∈ s.acks, r.ts = 3 := by
  have h₁ := BrokenReachable.next (.initial (f := .wallClock)) (.good (.lease {} 10 (by decide)))
  have h₂ := BrokenReachable.next h₁ (.good (.serveRead _ 7 rfl rfl (by decide) (by decide)))
  have h₃ := BrokenReachable.next h₂ (.good (.crash _))
  have h₄ := BrokenReachable.next h₃ (.wallRestart _ 1 rfl)
  have h₅ := broken_write h₄ ⟨3, .createInstance 0⟩ rfl rfl (by decide)
  exact ⟨_, h₅, by simp [writeRecord, restartState], ⟨3, .createInstance 0⟩, by simp [writeRecord, restartState], rfl⟩

def ddlRecord : Record := ⟨3, .ddl 0 [1] [(⟨0, 0⟩, some 7)] 1 1⟩

/-- Refutes replay equivalence: durable resolved backfill 7 is replayed, then
its source SQL (v := v+1) is incorrectly run again, recovering 8. -/
theorem rerun_backfill_changes_rows : ∃ s, BrokenReachable .rerunBackfill s ∧
    (s.visible.databases 0).bind (fun b => b.rows ⟨0, 0⟩) = some 8 ∧
    ((replay {} s.disk.log).databases 0).bind (fun b => b.rows ⟨0, 0⟩) = some 7 := by
  have h₁ := writeRecord_reachable .initial bootRecord rfl rfl (by decide)
  have hb := writeRecord_reachable h₁ ⟨2, .txn 0 [(⟨0, 0⟩, some 6)]⟩ rfl rfl (by decide)
  have h₂ := writeRecord_reachable hb ddlRecord rfl rfl (by decide)
  have h₃ := BrokenReachable.next (lift_reachable (f := .rerunBackfill) h₂) (.good (.crash _))
  have h₄ := BrokenReachable.next h₃ (.rerun _ 0 rfl)
  exact ⟨_, h₄, rfl, rfl⟩

def publishedBoot : State :=
  { boot with disk := { boot.disk with checkpoint := { boundary := 1, clockHigh := 1, image := boot.visible, past := boot.disk.log }, wal := [] }, scratch := none }

theorem published_boot_reachable : Reachable publishedBoot := by
  have h₁ : Reachable boot := writeRecord_reachable .initial bootRecord rfl rfl (by decide)
  have h₂ := Reachable.next h₁ (.checkpointStart _ rfl rfl)
  exact .next h₂ (.publish _ _ [] rfl (by simp [boot, writeRecord, Disk.log]) (by decide))

/-- Refutes (c) if boundary filtering is removed: the physically retained CREATE
runs twice and advances the catalog allocator twice. Replay need not be idempotent. -/
theorem covered_wal_replay_is_wrong : ∃ s, BrokenReachable .ignoreBoundary s ∧
    s.visible.allocator = 2 ∧ s.disk.restore.allocator = 1 := by
  have h₁ := BrokenReachable.next (lift_reachable (f := .ignoreBoundary) published_boot_reachable) (.good (.crash _))
  have h₂ := BrokenReachable.next h₁ (.unfiltered _ 0 rfl)
  exact ⟨_, h₂, rfl, rfl⟩

/-- Refutes the chosen global-order strengthening: separate database locks let
physical WAL timestamps become [1,3,2]. Per-database order alone is weaker. -/
theorem per_database_gate_reorders_wal : ∃ s, BrokenReachable .perDatabaseGate s ∧
    ¬s.disk.log.Pairwise (fun a b => a.ts < b.ts) := by
  have hb : Reachable boot := writeRecord_reachable .initial bootRecord rfl rfl (by decide)
  let r : Record := ⟨2, .txn 0 []⟩
  have h₁ := BrokenReachable.next (lift_reachable (f := .perDatabaseGate) hb)
    (.good (.begin _ r rfl rfl (by decide)))
  have h₂ := BrokenReachable.next h₁ (.foreignAppend _ ⟨3, .txn 1 []⟩)
  have h₃ := BrokenReachable.next h₂ (.good (.finish _ r rfl))
  have h₄ := BrokenReachable.next h₃ (.good (.fsync _ r rfl))
  exact ⟨_, h₄, by decide⟩

inductive BrokenSequenceMove : SequenceState → SequenceState → Prop where
  | good {s t : SequenceState} : SequenceMove s t → BrokenSequenceMove s t
  | reset (s : SequenceState) : BrokenSequenceMove s { s with next := 0 }

inductive BrokenSequenceReachable : SequenceState → Prop where
  | initial : BrokenSequenceReachable {}
  | next {s t : SequenceState} : BrokenSequenceReachable s → BrokenSequenceMove s t → BrokenSequenceReachable t

/-- Refutes (e): reserve [0,2), issue 0, reset cursor on restart, issue 0 again. -/
theorem cursor_reset_reissues : ∃ s, BrokenSequenceReachable s ∧ ¬s.issued.Nodup := by
  have h₁ := BrokenSequenceReachable.next .initial (.good (.reserve {} 2 (by decide)))
  have h₂ := BrokenSequenceReachable.next h₁ (.good (.issue _ (by decide)))
  have hc := BrokenSequenceReachable.next h₂ (.good (.crash _))
  have h₃ := BrokenSequenceReachable.next hc (.reset _)
  have h₄ := BrokenSequenceReachable.next h₃ (.good (.issue _ (by decide)))
  exact ⟨_, h₄, by decide⟩

/-- Non-vacuity: acknowledged data, nonzero published boundary, crash and restart. -/
example : Reachable publishedBoot ∧ publishedBoot.acks ≠ [] ∧
    publishedBoot.disk.checkpoint.boundary > 0 ∧
    Move publishedBoot (crashed publishedBoot) ∧
    Reachable (restartState (crashed publishedBoot) 0) := by
  refine ⟨published_boot_reachable, by decide, by decide, .crash _, ?_⟩
  exact .next (.next published_boot_reachable (.crash _)) (.restart _ 0 rfl rfl rfl)

/-- A nonempty Target product history survives its actual storage crash. -/
example : ∃ s, ProductReachable {} s ∧ s.history.length = 1 ∧
    s.storage.running = false ∧ s.target.log.length = 1 := by
  let c : Commit := ⟨1, 1, [], [], [], {}⟩
  have h₁ := ProductReachable.next (.initial (cfg := {}))
    (.call {} ⟨1, .beginRW⟩ rfl rfl rfl)
  have h₂ := ProductReachable.next h₁ (.begin _ 1 [] c 2 rfl rfl (by decide) rfl)
  have h₃ := ProductReachable.next h₂ (.finish _ rfl)
  have h₄ := ProductReachable.next h₃ (.persist _ true rfl)
  have h₅ := ProductReachable.next h₄ (.flush _ rfl)
  have hl := ProductReachable.next h₅ (.storage _ _ (.lease _ 10 (by decide)) rfl rfl rfl)
  have h₆ := ProductReachable.next hl (.ack _ rfl (by decide))
  have h₇ := ProductReachable.next h₆ (.crash _)
  exact ⟨_, h₇, rfl, rfl, rfl⟩

#print axioms durability
#print axioms atomicity
#print axioms log_monotone
#print axioms physical_recovery
#print axioms next_sequence_physical
#print axioms checkpoint_safety
#print axioms served_within_lease
#print axioms acknowledged_within_lease
#print axioms every_post_restart_timestamp
#print axioms storage_sequences_never_reissue
#print axioms acknowledged_drop_recovered
#print axioms instance_drop_survives
#print axioms product_target_inv
#print axioms product_recovered_rows
#print axioms product_recovered_target_rows
#print axioms product_visible_rows
#print axioms product_timestamp_mapping
#print axioms target_serializable_across_restarts
#print axioms early_ack_loses_durability
#print axioms early_truncate_loses_recovery
#print axioms wall_restart_regresses
#print axioms rerun_backfill_changes_rows
#print axioms covered_wal_replay_is_wrong
#print axioms per_database_gate_reorders_wal
#print axioms cursor_reset_reissues

end TxnSpec.Persistence
