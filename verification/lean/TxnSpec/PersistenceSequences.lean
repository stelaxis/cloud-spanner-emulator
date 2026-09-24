import TxnSpec.Persistence

namespace TxnSpec.Persistence

/-! Sequence allocation is a projection of the storage machine: the durable
reservation end is part of Image and every checkpoint. The coupled allocator
exposes a new range only after fsync and installation. `issue` is independent of transaction
commit/abort and therefore includes queries and aborted transactions. -/
structure SequenceState where
  high : Nat := 0 -- durable exclusive reservation end
  next : Nat := 0 -- volatile cursor
  issued : List Nat := [] -- ghost client observations

inductive SequenceMove : SequenceState → SequenceState → Prop where
  | reserve (s : SequenceState) (n : Nat) (h : s.high ≤ n) :
      SequenceMove s { s with high := n }
  | issue (s : SequenceState) (h : s.next < s.high) :
      SequenceMove s { s with next := s.next + 1, issued := s.issued ++ [s.next] }
  | crash (s : SequenceState) : SequenceMove s { s with next := s.high }
  | abortOrQuery (s : SequenceState) : SequenceMove s s

structure SequenceInv (s : SequenceState) : Prop where
  bound : s.next ≤ s.high
  used : ∀ n ∈ s.issued, n < s.next
  unique : s.issued.Nodup

theorem sequence_inv_move {s t : SequenceState} (h : SequenceInv s)
    (m : SequenceMove s t) : SequenceInv t := by
  cases m with
  | reserve n hn => exact { h with bound := Nat.le_trans h.bound hn }
  | issue hn =>
      refine ⟨by dsimp; omega, ?_, ?_⟩
      · intro n hm
        rcases List.mem_append.mp hm with hm | hm
        · have := h.used n hm; dsimp; omega
        · have := List.mem_singleton.mp hm; dsimp; omega
      · rw [List.nodup_append]
        refine ⟨h.unique, by simp, ?_⟩
        intro n hm k hk
        have he := List.mem_singleton.mp hk
        have := h.used n hm
        omega
  | crash =>
      refine ⟨Nat.le_refl _, ?_, h.unique⟩
      intro n hn
      have := h.used n hn
      exact Nat.lt_of_lt_of_le this h.bound
  | abortOrQuery => exact h

inductive SequenceReachable : SequenceState → Prop where
  | initial : SequenceReachable {}
  | next {s t : SequenceState} : SequenceReachable s → SequenceMove s t → SequenceReachable t

theorem sequence_inv {s : SequenceState} (h : SequenceReachable s) : SequenceInv s := by
  induction h with
  | initial => constructor <;> simp
  | next _ m ih => exact sequence_inv_move ih m

/-- (e) No value handed out by this sequence incarnation is issued twice,
regardless of crash/abort/query interleavings or gaps in reserved ranges. -/
theorem sequences_never_reissue {s : SequenceState} (h : SequenceReachable s) :
    s.issued.Nodup := (sequence_inv h).unique

/-- Link to the physical replay projection: reservation records can only raise
high-water marks; DDL and drops never reset a sequence incarnation's mark. -/
theorem effect_sequence_monotone (s : Image) (e : Effect) (q : Nat) :
    s.sequences q ≤ (applyEffect s e).sequences q := by
  cases e <;> simp [applyEffect]
  rename_i i n
  by_cases hi : q = i
  · subst q; simpa only [upd_same] using Nat.le_max_left (s.sequences i) n
  · simp [upd, hi]

theorem replay_sequence_monotone (s : Image) (rs : List Record) (q : Nat) :
    s.sequences q ≤ (replay s rs).sequences q := by
  induction rs generalizing s with
  | nil => exact Nat.le_refl _
  | cons r rs ih => exact Nat.le_trans (effect_sequence_monotone s r.effect q) (ih (applyRecord s r))

/-- Durable sequence projection of the full WAL/checkpoint machine. -/
def durableHigh (s : State) (q : Nat) : Nat := (replay {} s.disk.log).sequences q

theorem storage_high_monotone {s t : State} (m : Move s t) (q : Nat) :
    durableHigh s q ≤ durableHigh t q := by
  cases m with
  | persist r hp =>
      simp only [durableHigh, Disk.log, ← List.append_assoc, replay_append]
      exact replay_sequence_monotone _ [r] q
  | fsync r hp =>
      simp only [durableHigh, Disk.log, ← List.append_assoc, replay_append]
      exact replay_sequence_monotone _ [r] q
  | publish snap tail hs hp hb => simp only [durableHigh, Disk.log, ← hp, Nat.le_refl]
  | _ => exact Nat.le_refl _

structure Allocator where
  machine : State := {}
  cursor : Nat := 0
  issued : List Nat := [] -- ghost, not a disk field

def Allocator.project (s : Allocator) (q : Nat) : SequenceState :=
  { high := durableHigh s.machine q, next := s.cursor, issued := s.issued }

/-- The allocator is coupled to EVERY storage transition, including incomplete
reservation appends, checkpoint publication and crashes. No transaction abort
can undo an issuance. A cold machine abandons the rest of its durable range. -/
inductive AllocatorMove (q : Nat) : Allocator → Allocator → Prop where
  | storage (s : Allocator) (t : State) (m : Move s.machine t) :
      AllocatorMove q s { s with machine := t, cursor := if t.running then s.cursor else t.disk.restore.sequences q }
  | issue (s : Allocator) (hr : s.machine.running = true)
      (hg : s.machine.pending = .idle ∨ ∃ r, s.machine.pending = .installed r)
      (hn : s.cursor < s.machine.visible.sequences q) :
      AllocatorMove q s { s with cursor := s.cursor + 1, issued := s.issued ++ [s.cursor] }

inductive AllocatorReachable (q : Nat) : Allocator → Prop where
  | initial : AllocatorReachable q {}
  | next {s t : Allocator} : AllocatorReachable q s → AllocatorMove q s t → AllocatorReachable q t

theorem allocator_refines (q : Nat) {s : Allocator} (h : AllocatorReachable q s) :
    Reachable s.machine ∧ SequenceReachable (s.project q) := by
  induction h with
  | initial => exact ⟨.initial, .initial⟩
  | @next s t _ m ih =>
      cases m with
      | storage t hm =>
          refine ⟨.next ih.1 hm, ?_⟩
          have hs := SequenceReachable.next ih.2
            (SequenceMove.reserve (s.project q) (durableHigh t q) (storage_high_monotone hm q))
          by_cases hr : t.running = true
          · simpa [Allocator.project, hr] using hs
          · have hc := SequenceReachable.next hs (SequenceMove.crash _)
            simpa [Allocator.project, hr, durableHigh, restore_eq (reachable_inv (.next ih.1 hm))] using hc
      | issue hr hg hn =>
          refine ⟨ih.1, ?_⟩
          have hv := (reachable_invariants ih.1).2.1 hr
          have he : s.machine.visible = replay {} s.machine.disk.log := by
            rcases hg with hi | ⟨r, hi⟩ <;> simpa [hi] using hv
          have hb : s.cursor < durableHigh s.machine q := by simpa [durableHigh, he] using hn
          exact .next ih.2 (SequenceMove.issue (s.project q) hb)

/-- (e), strengthened: no reissue in the actual storage/allocator product,
for arbitrary record writes, fsync windows, checkpoints and restart schedules. -/
theorem storage_sequences_never_reissue (q : Nat) {s : Allocator}
    (h : AllocatorReachable q s) : s.issued.Nodup :=
  sequences_never_reissue (allocator_refines q h).2

end TxnSpec.Persistence
