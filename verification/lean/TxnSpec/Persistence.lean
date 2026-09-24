import TxnSpec.TargetProofs

/-! Storage protocol specification, not byte encoding/filesystem verification.
`Snapshot.past` is ghost history: only `image` and `boundary` are stored.
Records contain resolved physical effects; replay never executes SQL/backfills.
An append has writing, complete, durable, synced, and acknowledged stages.
Checkpoint publication switches the manifest atomically; a single physical WAL
retains covered records until truncation. Any stage can be followed by a crash. -/
namespace TxnSpec.Persistence

structure Database where
  rows : View := emptyView
  schema : List Nat := []
  tableAllocator : Nat := 0
  columnAllocator : Nat := 0

structure Image where
  instances : Nat → Bool := fun _ => false
  databases : Nat → Option Database := fun _ => none
  owner : Nat → Nat := fun _ => 0
  allocator : Nat := 0
  sequences : Nat → Nat := fun _ => 0
  clock : Nat := 0

inductive Effect where
  | txn (db : Nat) (writes : Writes)
  | ddl (db : Nat) (schema : List Nat) (backfill : Writes) (tables columns : Nat)
  | createInstance (id : Nat)
  | dropInstance (id : Nat)
  | createDatabase (id parent : Nat)
  | dropDatabase (id : Nat)
  | reserve (sequence upper : Nat)
deriving DecidableEq

structure Record where
  ts : Nat
  effect : Effect
deriving DecidableEq

def applyEffect (s : Image) : Effect → Image
  | .txn d ws => { s with databases := upd s.databases d ((s.databases d).map fun b => { b with rows := overlay ws b.rows }) }
  | .ddl d schema ws t c => { s with databases := upd s.databases d ((s.databases d).map fun b => { b with rows := overlay ws b.rows, schema, tableAllocator := t, columnAllocator := c }) }
  | .createInstance i => { s with instances := upd s.instances i true }
  | .dropInstance i => { s with instances := upd s.instances i false, databases := fun d => if s.owner d = i then none else s.databases d }
  | .createDatabase d i => { s with databases := upd s.databases d (some {}), owner := upd s.owner d i, allocator := s.allocator + 1 }
  | .dropDatabase d => { s with databases := upd s.databases d none }
  | .reserve q n => { s with sequences := upd s.sequences q (max (s.sequences q) n) }

def applyRecord (s : Image) (r : Record) : Image :=
  { applyEffect s r.effect with clock := max s.clock r.ts }

def replay (s : Image) : List Record → Image
  | [] => s
  | r :: rs => replay (applyRecord s r) rs

theorem replay_append (s : Image) (xs ys : List Record) :
    replay s (xs ++ ys) = replay (replay s xs) ys := by
  induction xs generalizing s with
  | nil => rfl
  | cons r rs ih => exact ih _

theorem replay_clock_ge (s : Image) (rs : List Record) : s.clock ≤ (replay s rs).clock := by
  induction rs generalizing s with
  | nil => exact Nat.le_refl _
  | cons r rs ih => exact Nat.le_trans (Nat.le_max_left _ _) (ih (applyRecord s r))

theorem replay_clock_record (s : Image) (rs : List Record) (r : Record) (h : r ∈ rs) :
    r.ts ≤ (replay s rs).clock := by
  induction rs generalizing s with
  | nil => simp at h
  | cons c cs ih =>
      rcases List.mem_cons.mp h with rfl | hm
      · exact Nat.le_trans (Nat.le_max_right _ _) (replay_clock_ge (applyRecord s r) cs)
      · exact ih _ hm


structure Snapshot where
  boundary : Nat := 0
  clockHigh : Nat := 0
  image : Image := {}
  past : List Record := [] -- ghost only

structure WALEntry where
  seq : Nat
  record : Record
 deriving DecidableEq

def numbered (n : Nat) : List Record → List WALEntry
  | [] => []
  | r :: rs => ⟨n, r⟩ :: numbered (n + 1) rs

def suffix (n : Nat) (xs : List WALEntry) := xs.filter (fun e => decide (n ≤ e.seq))

theorem numbered_append (n : Nat) (xs ys : List Record) :
    numbered n (xs ++ ys) = numbered n xs ++ numbered (n + xs.length) ys := by
  induction xs generalizing n with
  | nil => simp [numbered]
  | cons x xs ih => simp [numbered, ih, Nat.add_comm, Nat.add_left_comm]

theorem suffix_nested (a b : Nat) (h : a ≤ b) (xs : List WALEntry) :
    suffix b (suffix a xs) = suffix b xs := by
  simp only [suffix, List.filter_filter]
  congr 1
  funext e
  apply Bool.eq_iff_iff.mpr
  simp only [Bool.and_eq_true, decide_eq_true_eq]
  omega

theorem numbered_suffix (xs : List Record) (n b : Nat) :
    (suffix b (numbered n xs)).map WALEntry.record = xs.drop (b - n) := by
  induction xs generalizing n with
  | nil => simp [numbered, suffix]
  | cons x xs ih =>
      by_cases h : b ≤ n
      · have hn : b - n = 0 := by omega
        have hn' : b - (n + 1) = 0 := by omega
        simpa [numbered, suffix, h, hn, hn'] using ih (n + 1)
      · have hn : b - n = (b - (n + 1)) + 1 := by omega
        simpa [numbered, suffix, h, hn] using ih (n + 1)

structure Disk where
  checkpoint : Snapshot := {}
  wal : List Record := [] -- ghost logical suffix, NOT another physical file
  physical : List WALEntry := [] -- the ONE physical WAL
  lease : Nat := 0 -- fsynced upper bound on timestamps that may be served
  tornTail : Bool := false
  corrupt : Bool := false

def Disk.log (d : Disk) := d.checkpoint.past ++ d.wal

def Disk.restore (d : Disk) : Image := replay d.checkpoint.image ((suffix d.checkpoint.boundary d.physical).map WALEntry.record)

def Disk.recover (d : Disk) : Except String Image :=
  if d.corrupt then .error "corrupt checkpoint/WAL (not a torn final record)"
  else .ok d.restore

inductive Pending where
  | idle
  | writing (r : Record)
  | complete (r : Record)
  | durable (r : Record) (synced : Bool)
  | installed (r : Record)

structure State where
  disk : Disk := {}
  visible : Image := {} -- volatile, atomically installed after fsync
  pending : Pending := .idle
  scratch : Option Snapshot := none
  acks : List Record := [] -- ghost client observations, survive crashes
  running : Bool := true
  clock : Nat := 0
  restartBoundary : Nat := 0
  served : List Nat := [] -- ghost read timestamp observations

/-- Clearing tornTail obliges the implementation to physically truncate torn
bytes BEFORE the next append. It is not permission to append after torn bytes. -/
def restartState (s : State) (wall : Nat) : State :=
  { s with running := true, visible := s.disk.restore, disk := { s.disk with tornTail := false }, clock := max (max (max s.disk.checkpoint.clockHigh s.disk.lease) s.disk.restore.clock) wall + 1, restartBoundary := max (max (max s.disk.checkpoint.clockHigh s.disk.lease) s.disk.restore.clock) wall + 1 }

/-- One append/commit critical section. Complete records may reach durable
storage before fsync; a torn tail never becomes a replayable record. -/
inductive Move : State → State → Prop where
  | begin (s : State) (r : Record) (hi : s.pending = .idle) (hr : s.running = true)
      (hc : s.clock < r.ts) :
      Move s { s with pending := .writing r, clock := r.ts }
  | bytes (s : State) (r : Record) (h : s.pending = .writing r) :
      Move s { s with disk := { s.disk with tornTail := true } }
  | finish (s : State) (r : Record) (h : s.pending = .writing r) :
      Move s { s with pending := .complete r }
  | persist (s : State) (r : Record) (h : s.pending = .complete r) :
      Move s { s with disk := { s.disk with wal := s.disk.wal ++ [r], physical := s.disk.physical ++ [⟨s.disk.log.length, r⟩], tornTail := false }, pending := .durable r false }
  | fsync (s : State) (r : Record) (h : s.pending = .complete r) :
      Move s { s with disk := { s.disk with wal := s.disk.wal ++ [r], physical := s.disk.physical ++ [⟨s.disk.log.length, r⟩], tornTail := false }, pending := .durable r true }
  | syncPersisted (s : State) (r : Record) (h : s.pending = .durable r false) :
      Move s { s with pending := .durable r true }
  | flush (s : State) (r : Record) (h : s.pending = .durable r true) :
      Move s { s with pending := .installed r, visible := applyRecord s.visible r }
  | ack (s : State) (r : Record) (h : s.pending = .installed r)
      (hl : r.ts ≤ s.disk.lease) :
      Move s { s with pending := .idle, acks := s.acks ++ [r] }
  | checkpointStart (s : State) (hi : s.pending = .idle) (hr : s.running = true) :
      Move s { s with scratch := some { boundary := s.disk.checkpoint.boundary + s.disk.wal.length, clockHigh := s.clock, image := s.visible, past := s.disk.log } }
  | checkpointBytes (s : State) : Move s s
  /-- Atomic durable manifest publication, after snapshot fsync. The boundary
  may precede concurrent WAL appends. Covered WAL is still physically present. -/
  | publish (s : State) (snap : Snapshot) (tail : List Record)
      (hs : s.scratch = some snap) (hp : s.disk.log = snap.past ++ tail)
      (hb : s.disk.checkpoint.boundary ≤ snap.boundary) :
      Move s { s with disk := { s.disk with checkpoint := snap, wal := tail }, scratch := none }
  | truncate (s : State) (upto : Nat) (h : upto ≤ s.disk.checkpoint.boundary) :
      Move s { s with disk := { s.disk with physical := suffix upto s.disk.physical } }
  | lease (s : State) (upper : Nat) (h : s.disk.lease ≤ upper) :
      Move s { s with disk := { s.disk with lease := upper } }
  | serveRead (s : State) (ts : Nat) (hi : s.pending = .idle)
      (hr : s.running = true) (hc : s.clock ≤ ts) (hl : ts ≤ s.disk.lease) :
      Move s { s with clock := ts, served := s.served ++ [ts] }
  | crash (s : State) :
      Move s { s with pending := .idle, visible := {}, scratch := none, running := false, clock := 0, restartBoundary := 0 }
  | restart (s : State) (wall : Nat) (h : s.running = false) (hi : s.pending = .idle)
      (hc : s.disk.corrupt = false) :
      Move s (restartState s wall)
  | corruption (s : State) : Move s { s with disk := { s.disk with corrupt := true } }

def PendingOK (p : Pending) (log : List Record) : Prop :=
  match p with
  | .idle => True
  | .writing r | .complete r => ∀ c ∈ log, c.ts < r.ts
  | .durable r _ | .installed r => r ∈ log

structure Inv (s : State) : Prop where
  snapshot : s.disk.checkpoint.image = replay {} s.disk.checkpoint.past
  boundary : s.disk.checkpoint.boundary = s.disk.checkpoint.past.length
  acked : ∀ r ∈ s.acks, r ∈ s.disk.log
  ordered : s.disk.log.Pairwise (fun a b => a.ts < b.ts)
  pending : PendingOK s.pending s.disk.log
  physical : suffix s.disk.checkpoint.boundary s.disk.physical =
    suffix s.disk.checkpoint.boundary (numbered 0 s.disk.log)
  scratch : ∀ snap, s.scratch = some snap →
    snap.image = replay {} snap.past ∧ snap.boundary = snap.past.length

theorem inv_init : Inv {} := by
  constructor <;> simp [Disk.log, PendingOK, replay, suffix, numbered]

theorem restore_eq {s : State} (h : Inv s) : s.disk.restore = replay {} s.disk.log := by
  unfold Disk.restore
  rw [h.physical, numbered_suffix]
  simp [h.boundary, Disk.log, h.snapshot, replay_append]

/-- Live data is installed atomically. During the durable-before-flush window
reads/checkpoints must wait on the commit gate. -/
def VisibleInv (s : State) : Prop :=
  s.running = true → match s.pending with
  | .durable r _ => ∃ prior, s.disk.log = prior ++ [r] ∧ s.visible = replay {} prior
  | _ => s.visible = replay {} s.disk.log

theorem visible_init : VisibleInv {} := by intro _; rfl

theorem visible_move {s t : State} (hs : Inv s) (h : VisibleInv s) (m : Move s t) : VisibleInv t := by
  cases m with
  | begin r hi _ _ => simpa [VisibleInv, hi] using h
  | bytes => exact h
  | finish r hp => simpa [VisibleInv, hp] using h
  | persist r hp =>
      intro hr
      refine ⟨s.disk.log, ?_, ?_⟩
      · simp [Disk.log, List.append_assoc]
      · simpa [VisibleInv, hp] using h hr
  | fsync r hp =>
      intro hr
      refine ⟨s.disk.log, ?_, ?_⟩
      · simp [Disk.log, List.append_assoc]
      · simpa [VisibleInv, hp] using h hr
  | syncPersisted r hp => simpa [VisibleInv, hp] using h
  | flush r hp =>
      intro hr
      have hv := h hr
      simp only [hp] at hv
      obtain ⟨prior, hl, hi⟩ := hv
      change applyRecord s.visible r = replay {} s.disk.log
      rw [hi, hl, replay_append]
      rfl
  | ack r hp hl => simpa [VisibleInv, hp] using h
  | checkpointStart => exact h
  | checkpointBytes => exact h
  | publish snap tail hs hp hb => simpa only [VisibleInv, Disk.log, ← hp] using h
  | truncate => exact h
  | lease => exact h
  | serveRead => exact h
  | crash => simp [VisibleInv]
  | restart wall hr hi hc =>
      simpa [VisibleInv, restartState, hi, Disk.log] using restore_eq hs
  | corruption => exact h

def PendingClock (s : State) : Prop :=
  match s.pending with
  | .idle => True
  | .writing r | .complete r | .durable r _ | .installed r =>
    s.running = true ∧ r.ts ≤ s.clock

structure ClockInv (s : State) : Prop where
  live : s.running = true → ∀ r ∈ s.disk.log, r.ts ≤ s.clock
  pending : PendingClock s

theorem clock_init : ClockInv {} := by constructor <;> simp [Disk.log, PendingClock]

theorem clock_move {s t : State} (hs : Inv s) (h : ClockInv s) (m : Move s t) : ClockInv t := by
  cases m with
  | begin r hi hr hc =>
      refine ⟨?_, hr, Nat.le_refl _⟩
      intro _ c hm
      have := h.live hr c hm
      dsimp; omega
  | bytes => exact ⟨h.live, h.pending⟩
  | finish r hp => exact ⟨h.live, by simpa [PendingClock, hp] using h.pending⟩
  | persist r hp =>
      have hr : s.running = true ∧ r.ts ≤ s.clock := by simpa [PendingClock, hp] using h.pending
      refine ⟨?_, hr⟩
      intro hh c hm
      have hm' : c ∈ s.disk.log ++ [r] := by simpa [Disk.log, List.append_assoc] using hm
      rcases List.mem_append.mp hm' with hm | hm
      · exact h.live hh c hm
      · have := List.mem_singleton.mp hm; subst c; exact hr.2
  | fsync r hp =>
      have hr : s.running = true ∧ r.ts ≤ s.clock := by simpa [PendingClock, hp] using h.pending
      refine ⟨?_, hr⟩
      intro hh c hm
      have hm' : c ∈ s.disk.log ++ [r] := by simpa [Disk.log, List.append_assoc] using hm
      rcases List.mem_append.mp hm' with hm | hm
      · exact h.live hh c hm
      · have := List.mem_singleton.mp hm; subst c; exact hr.2
  | syncPersisted r hp => exact ⟨h.live, by simpa [PendingClock, hp] using h.pending⟩
  | flush r hp => exact ⟨h.live, by simpa [PendingClock, hp] using h.pending⟩
  | ack => exact ⟨h.live, trivial⟩
  | checkpointStart => exact ⟨h.live, h.pending⟩
  | checkpointBytes => exact h
  | publish snap tail hs hp hb =>
      exact ⟨by simpa only [Disk.log, ← hp] using h.live, h.pending⟩
  | truncate => exact ⟨h.live, h.pending⟩
  | lease => exact ⟨h.live, h.pending⟩
  | serveRead ts hi hr hc hl =>
      exact ⟨fun hh r hm => Nat.le_trans (h.live hh r hm) hc, by simp [PendingClock, hi]⟩
  | crash => exact ⟨by simp, trivial⟩
  | restart wall hr hi hc =>
      refine ⟨?_, by simp [PendingClock, restartState, hi]⟩
      intro _ r hm
      have hb := replay_clock_record {} s.disk.log r hm
      have h₁ := Nat.le_max_right (max s.disk.checkpoint.clockHigh s.disk.lease) (replay {} s.disk.log).clock
      have h₂ := Nat.le_max_left (max (max s.disk.checkpoint.clockHigh s.disk.lease) (replay {} s.disk.log).clock) wall
      dsimp only [restartState]
      rw [restore_eq hs]
      omega
  | corruption => exact ⟨h.live, h.pending⟩

theorem inv_move {s t : State} (h : Inv s) (hv : VisibleInv s) (hk : ClockInv s) (m : Move s t) : Inv t := by
  cases m with
  | begin r _ hr hc =>
      refine { h with pending := ?_ }
      intro c hm
      have := hk.live hr c hm
      omega
  | bytes r hp => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩
  | finish r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | persist r hp =>
      have ho : ∀ c ∈ s.disk.log, c.ts < r.ts := by simpa [hp, PendingOK] using h.pending
      have hl : s.disk.checkpoint.past ++ (s.disk.wal ++ [r]) = s.disk.log ++ [r] :=
        (List.append_assoc _ _ _).symm
      refine ⟨h.snapshot, h.boundary, ?_, ?_, ?_, ?_, h.scratch⟩
      · intro c hc
        change c ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; exact List.mem_append_left _ (h.acked c hc)
      · change (s.disk.checkpoint.past ++ (s.disk.wal ++ [r])).Pairwise _
        rw [hl, List.pairwise_append]
        exact ⟨h.ordered, by simp, by simpa using ho⟩
      · change r ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; simp
      · change suffix s.disk.checkpoint.boundary (s.disk.physical ++ [⟨s.disk.log.length, r⟩]) = _
        change suffix s.disk.checkpoint.boundary (s.disk.physical ++ [⟨s.disk.log.length, r⟩]) =
          suffix s.disk.checkpoint.boundary (numbered 0 (s.disk.checkpoint.past ++ (s.disk.wal ++ [r])))
        rw [hl, numbered_append]
        simp only [Nat.zero_add, numbered, suffix, List.filter_append]
        exact congrArg (· ++ suffix s.disk.checkpoint.boundary [⟨s.disk.log.length, r⟩]) h.physical
  | fsync r hp =>
      have ho : ∀ c ∈ s.disk.log, c.ts < r.ts := by simpa [hp, PendingOK] using h.pending
      have hl : s.disk.checkpoint.past ++ (s.disk.wal ++ [r]) = s.disk.log ++ [r] :=
        (List.append_assoc _ _ _).symm
      refine ⟨h.snapshot, h.boundary, ?_, ?_, ?_, ?_, h.scratch⟩
      · intro c hc
        change c ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; exact List.mem_append_left _ (h.acked c hc)
      · change (s.disk.checkpoint.past ++ (s.disk.wal ++ [r])).Pairwise _
        rw [hl, List.pairwise_append]
        exact ⟨h.ordered, by simp, by simpa using ho⟩
      · change r ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; simp
      · change suffix s.disk.checkpoint.boundary (s.disk.physical ++ [⟨s.disk.log.length, r⟩]) = _
        change suffix s.disk.checkpoint.boundary (s.disk.physical ++ [⟨s.disk.log.length, r⟩]) =
          suffix s.disk.checkpoint.boundary (numbered 0 (s.disk.checkpoint.past ++ (s.disk.wal ++ [r])))
        rw [hl, numbered_append]
        simp only [Nat.zero_add, numbered, suffix, List.filter_append]
        exact congrArg (· ++ suffix s.disk.checkpoint.boundary [⟨s.disk.log.length, r⟩]) h.physical
  | syncPersisted r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | flush r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | ack r hp hl =>
      have hm : r ∈ s.disk.log := by simpa [hp, PendingOK] using h.pending
      refine { h with pending := trivial, acked := ?_ }
      intro c hc
      rcases List.mem_append.mp hc with hc | hc
      · exact h.acked c hc
      · have he := List.mem_singleton.mp hc; subst c; exact hm
  | checkpointStart hi hr =>
      refine { h with scratch := ?_ }
      intro snap hs
      cases Option.some.inj hs
      exact ⟨by simpa [VisibleInv, hi] using hv hr, by simp [Disk.log, h.boundary]⟩
  | checkpointBytes => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩
  | publish snap tail hs hp hb =>
      obtain ⟨hi, hb⟩ := h.scratch snap hs
      refine ⟨hi, hb, ?_, ?_, ?_, ?_, ?_⟩
      · simpa only [Disk.log, ← hp] using h.acked
      · simpa only [Disk.log, ← hp] using h.ordered
      · simpa only [Disk.log, ← hp] using h.pending
      · change suffix snap.boundary s.disk.physical = suffix snap.boundary (numbered 0 (snap.past ++ tail))
        rw [← hp, ← suffix_nested s.disk.checkpoint.boundary snap.boundary ‹s.disk.checkpoint.boundary ≤ snap.boundary›, h.physical,
          suffix_nested _ _ ‹s.disk.checkpoint.boundary ≤ snap.boundary›]
      · simp
  | truncate upto hn => exact { h with physical := by simpa only [Disk.log, suffix_nested _ _ hn] using h.physical }
  | lease => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩
  | serveRead => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩
  | crash => exact { h with pending := trivial, scratch := by simp }
  | restart => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩
  | corruption => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.physical, h.scratch⟩

inductive Reachable : State → Prop where
  | initial : Reachable {}
  | next {s t : State} : Reachable s → Move s t → Reachable t

theorem reachable_invariants {s : State} (h : Reachable s) : Inv s ∧ VisibleInv s ∧ ClockInv s := by
  induction h with
  | initial => exact ⟨inv_init, visible_init, clock_init⟩
  | next _ m ih => exact ⟨inv_move ih.1 ih.2.1 ih.2.2 m, visible_move ih.1 ih.2.1 m, clock_move ih.1 ih.2.2 m⟩

theorem reachable_inv {s : State} (h : Reachable s) : Inv s := (reachable_invariants h).1

/-- A quiescent snapshot captures actual installed data and its exact boundary,
not an idealized reconstruction substituted for the live data. -/
theorem quiescent_image {s : State} (h : Reachable s) (hi : s.pending = .idle)
    (hr : s.running = true) : s.visible = replay {} s.disk.log := by
  have hv := (reachable_invariants h).2.1 hr
  simpa [hi] using hv

/-- (a) Acknowledged records remain in the recovered logical history,
including DDL and catalog operations. Later writes/drops can supersede effects. -/
theorem durability {s : State} (h : Reachable s) :
    ∀ r ∈ s.acks, r ∈ s.disk.log := (reachable_inv h).acked

/-- (b) Replay exactly the complete, timestamp-ordered prefix. Incomplete
pending records/torn bytes are excluded; durable unacknowledged ones included. -/
theorem atomicity {s : State} (h : Reachable s) (hc : s.disk.corrupt = false) :
    s.disk.recover = .ok (replay {} s.disk.log) ∧
    s.disk.log.Pairwise (fun a b => a.ts < b.ts) := by
  have hi := reachable_inv h
  refine ⟨?_, hi.ordered⟩
  simp [Disk.recover, hc, restore_eq hi]

/-- (c) Scratch bytes are volatile; publication is atomic. This abstracts partial
snapshot writes by stuttering, rather than verifying byte-level I/O. -/
theorem checkpoint_safety {s t : State} (h : Reachable s) (m : Move s t)
    (hc : t.disk.corrupt = false) :
    (∀ r ∈ t.acks, r ∈ t.disk.log) ∧
    t.disk.recover = .ok (replay {} t.disk.log) ∧
    t.disk.log.Pairwise (fun a b => a.ts < b.ts) :=
  ⟨durability (.next h m), atomicity (.next h m) hc⟩

theorem corruption_fails (d : Disk) (h : d.corrupt = true) :
    d.recover = .error "corrupt checkpoint/WAL (not a torn final record)" := by
  simp [Disk.recover, h]

/-- (d) Every timestamp reserved after recovery exceeds this lower bound;
`Move.begin` enforces it and increases the running clock again. -/
theorem timestamp_restart (log : List Record) (wall ts : Nat)
    (h : max (replay {} log).clock wall + 1 ≤ ts) :
    wall < ts ∧ ∀ r ∈ log, r.ts < ts := by
  constructor
  · have := Nat.le_max_right (replay {} log).clock wall; omega
  · intro r hr
    have := replay_clock_record {} log r hr
    have := Nat.le_max_left (replay {} log).clock wall
    omega

def readAllowed (boundary ts : Nat) : Bool := decide (boundary ≤ ts)

theorem pre_restart_read_rejected (boundary ts : Nat) (h : ts < boundary) :
    readAllowed boundary ts = false := by simp [readAllowed]; omega

/-- A new database with the same display name must have a fresh incarnation. -/
def NoCreate (db : Nat) (r : Record) : Prop :=
  ∀ i, r.effect ≠ .createDatabase db i

theorem effect_keeps_dropped (s : Image) (db : Nat) (e : Effect)
    (h : s.databases db = none) (hn : ∀ i, e ≠ .createDatabase db i) :
    (applyEffect s e).databases db = none := by
  cases e <;> simp only [applyEffect]
  case txn d ws => by_cases hd : db = d <;> simp_all [upd]
  case ddl d sc ws t c => by_cases hd : db = d <;> simp_all [upd]
  case createInstance => exact h
  case dropInstance i => split <;> simp_all
  case createDatabase d i =>
    have hd : db ≠ d := by intro he; subst d; exact hn i rfl
    simp [upd, hd, h]
  case dropDatabase d => by_cases hd : db = d <;> simp_all [upd]
  case reserve => exact h

/-- (f) Updates, DDL and recovery cannot resurrect a dropped database. -/
theorem dropped_stays_dropped (s : Image) (db : Nat) (tail : List Record)
    (h : s.databases db = none) (hn : ∀ r ∈ tail, NoCreate db r) :
    (replay s tail).databases db = none := by
  induction tail generalizing s with
  | nil => exact h
  | cons r rs ih =>
      apply ih
      · exact effect_keeps_dropped s db r.effect h (hn r List.mem_cons_self)
      · intro c hc; exact hn c (List.mem_cons_of_mem _ hc)

/-- (f) in serial-log form: a complete durable DROP followed by any tail
without explicit recreation is absent after recovery, including checkpoints. -/
theorem drop_record_survives (prior tail : List Record) (db ts : Nat)
    (hn : ∀ r ∈ tail, NoCreate db r) :
    (replay {} (prior ++ ⟨ts, .dropDatabase db⟩ :: tail)).databases db = none := by
  rw [replay_append]
  apply dropped_stays_dropped _ db tail
  · simp [applyRecord, applyEffect]
  · exact hn

/-- Clock reservations cannot regress while the process stays live. -/
theorem live_clock_monotone {s t : State} (m : Move s t)
    (hs : s.running = true) (ht : t.running = true) : s.clock ≤ t.clock := by
  cases m <;> simp_all <;> omega

inductive LiveEpoch (start : State) : State → Prop where
  | initial (h : start.running = true) : LiveEpoch start start
  | next {s t : State} : LiveEpoch start s → Move s t → t.running = true → LiveEpoch start t

theorem epoch_clock {start s : State} (h : LiveEpoch start s) :
    start.clock ≤ s.clock ∧ s.running = true := by
  induction h with
  | initial hr => exact ⟨Nat.le_refl _, hr⟩
  | next _ m hr ih => exact ⟨Nat.le_trans ih.1 (live_clock_monotone m ih.2 hr), hr⟩

/-- Instance DROP also removes every database owned by that incarnation. -/
theorem instance_drop_survives (prior tail : List Record) (inst db ts : Nat)
    (ho : (replay {} prior).owner db = inst) (hn : ∀ r ∈ tail, NoCreate db r) :
    (replay {} (prior ++ ⟨ts, .dropInstance inst⟩ :: tail)).databases db = none := by
  rw [replay_append]
  apply dropped_stays_dropped _ db tail
  · simp [applyRecord, applyEffect, ho]
  · exact hn

/-- The durable logical log never rolls back, including crashes/publication. -/
theorem log_monotone {s t : State} (m : Move s t) : s.disk.log <+: t.disk.log := by
  cases m with
  | persist | fsync => simp [Disk.log, ← List.append_assoc]
  | publish snap tail hs hp hb => simpa only [Disk.log, ← hp] using (by simp : s.disk.log <+: s.disk.log)
  | _ => simp [Disk.log, restartState]

/-- Filtering the ONE physical WAL is equivalent to the logical suffix,
including after any number of checkpoints, truncations and crashes. -/
theorem physical_recovery {s : State} (h : Reachable s) :
    s.disk.restore = replay s.disk.checkpoint.image s.disk.wal ∧
    s.disk.restore = replay {} s.disk.log := by
  have hi := reachable_inv h
  constructor
  · unfold Disk.restore
    rw [hi.physical, numbered_suffix]
    simp [hi.boundary, Disk.log]
  · exact restore_eq hi

/-- All served read timestamps fit inside the durable clock lease. -/
def LeaseInv (s : State) := ∀ ts ∈ s.served, ts ≤ s.disk.lease

theorem lease_move {s t : State} (h : LeaseInv s) (m : Move s t) : LeaseInv t := by
  cases m with
  | lease upper hu => exact fun ts hm => Nat.le_trans (h ts hm) hu
  | serveRead ts hi hr hc hl =>
      intro n hn
      rcases List.mem_append.mp hn with hn | hn
      · exact h n hn
      · simpa only [List.mem_singleton.mp hn] using hl
  | _ => exact h

theorem served_within_lease {s : State} (h : Reachable s) : LeaseInv s := by
  induction h with
  | initial => simp [LeaseInv]
  | next _ m ih => exact lease_move ih m

/-- Acknowledged commit timestamps also fit the durable lease. -/
theorem acknowledged_within_lease {s : State} (h : Reachable s) :
    ∀ r ∈ s.acks, r.ts ≤ s.disk.lease := by
  induction h with
  | initial => simp
  | next _ m ih =>
      cases m with
      | lease upper hu => exact fun r hm => Nat.le_trans (ih r hm) hu
      | ack r hp hl =>
          intro c hm
          rcases List.mem_append.mp hm with hm | hm
          · exact ih c hm
          · simpa only [List.mem_singleton.mp hm] using hl
      | _ => exact ih

/-- The append sequence can be computed without ghost history: checkpoint
boundary plus the number of active physical records. -/
theorem next_sequence_physical {s : State} (h : Reachable s) :
    s.disk.log.length = s.disk.checkpoint.boundary +
      (suffix s.disk.checkpoint.boundary s.disk.physical).length := by
  have hi := reachable_inv h
  have hh := congrArg List.length (numbered_suffix s.disk.log 0 s.disk.checkpoint.boundary)
  rw [← hi.physical] at hh
  simp only [List.length_map, Nat.sub_zero, List.length_drop] at hh
  rw [hh, hi.boundary]
  simp [Disk.log]

/-- (d) Every commit reservation in the restarted epoch exceeds all durable
commit timestamps AND every read timestamp served before the crash. -/
theorem every_post_restart_timestamp (before : State) (hr : Reachable before)
    (wall : Nat) {s : State} (he : LiveEpoch (restartState before wall) s)
    (ts : Nat) (hc : s.clock < ts) :
    before.disk.checkpoint.clockHigh < ts ∧ before.disk.lease < ts ∧ wall < ts ∧
      (∀ r ∈ before.disk.log, r.ts < ts) ∧ (∀ read ∈ before.served, read < ts) := by
  have hb := (epoch_clock he).1
  change max (max (max before.disk.checkpoint.clockHigh before.disk.lease) before.disk.restore.clock) wall + 1 ≤ s.clock at hb
  have h₁ := Nat.le_max_left before.disk.checkpoint.clockHigh before.disk.lease
  have h₂ := Nat.le_max_right before.disk.checkpoint.clockHigh before.disk.lease
  have h₃ := Nat.le_max_left (max before.disk.checkpoint.clockHigh before.disk.lease) before.disk.restore.clock
  have h₄ := Nat.le_max_right (max before.disk.checkpoint.clockHigh before.disk.lease) before.disk.restore.clock
  have h₅ := Nat.le_max_left (max (max before.disk.checkpoint.clockHigh before.disk.lease) before.disk.restore.clock) wall
  have h₆ := Nat.le_max_right (max (max before.disk.checkpoint.clockHigh before.disk.lease) before.disk.restore.clock) wall
  refine ⟨by omega, by omega, by omega, ?_, ?_⟩
  · intro r hm
    have hh := replay_clock_record {} before.disk.log r hm
    rw [← restore_eq (reachable_inv hr)] at hh
    omega
  · intro read hm
    have hh := served_within_lease hr read hm
    omega

/-- (f) Reachability/acknowledgement glue: choose the acknowledged DROP's
position in the durable log; no later CREATE of this incarnation is allowed. -/
theorem acknowledged_drop_recovered {s : State} (h : Reachable s)
    (db ts : Nat) (ha : (⟨ts, .dropDatabase db⟩ : Record) ∈ s.acks)
    (hn : ∀ prior tail, s.disk.log = prior ++ ⟨ts, .dropDatabase db⟩ :: tail →
      ∀ r ∈ tail, NoCreate db r) :
    s.disk.restore.databases db = none := by
  have hm := durability h _ ha
  obtain ⟨prior, tail, he⟩ := List.mem_iff_append.mp hm
  rw [restore_eq (reachable_inv h), he]
  exact drop_record_survives prior tail db ts (hn prior tail he)

end TxnSpec.Persistence
