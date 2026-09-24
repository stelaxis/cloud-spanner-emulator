import TxnSpec.TargetProofs

/-! Storage protocol specification, not byte encoding/filesystem verification.
`Snapshot.past` is ghost history: only `image` and `boundary` are stored.
Records contain resolved physical effects; replay never executes SQL/backfills.
An append has writing, complete, durable, synced, and acknowledged stages.
Checkpoint publication switches the manifest atomically; obsolete WAL remains
in `garbage` until deletion. Any stage can be followed by a crash. -/
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
  | .createDatabase d i => { s with databases := upd s.databases d (some {}), owner := upd s.owner d i, allocator := max s.allocator (d + 1) }
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

structure Disk where
  checkpoint : Snapshot := {}
  wal : List Record := []
  garbage : List Record := [] -- ignored using the manifest sequence boundary
  tornTail : Bool := false
  corrupt : Bool := false

def Disk.log (d : Disk) := d.checkpoint.past ++ d.wal

def Disk.restore (d : Disk) : Image := replay d.checkpoint.image d.wal

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

def restartState (s : State) (wall : Nat) : State :=
  { s with running := true, visible := s.disk.restore, disk := { s.disk with tornTail := false }, clock := max (max s.disk.checkpoint.clockHigh s.disk.restore.clock) wall + 1, restartBoundary := max (max s.disk.checkpoint.clockHigh s.disk.restore.clock) wall + 1 }

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
      Move s { s with disk := { s.disk with wal := s.disk.wal ++ [r], tornTail := false }, pending := .durable r false }
  | fsync (s : State) (r : Record) (h : s.pending = .durable r false) :
      Move s { s with pending := .durable r true }
  | flush (s : State) (r : Record) (h : s.pending = .durable r true) :
      Move s { s with pending := .installed r, visible := applyRecord s.visible r }
  | ack (s : State) (r : Record) (h : s.pending = .installed r) :
      Move s { s with pending := .idle, acks := s.acks ++ [r] }
  | checkpointStart (s : State) (hi : s.pending = .idle) (hr : s.running = true) :
      Move s { s with scratch := some { boundary := s.disk.checkpoint.boundary + s.disk.wal.length, clockHigh := s.clock, image := s.visible, past := s.disk.log } }
  | checkpointBytes (s : State) : Move s s
  /-- Atomic durable manifest publication, after snapshot fsync. The boundary
  may precede concurrent WAL appends. Covered WAL is still physically present. -/
  | publish (s : State) (snap : Snapshot) (tail : List Record)
      (hs : s.scratch = some snap) (hp : s.disk.log = snap.past ++ tail) :
      Move s { s with disk := { s.disk with checkpoint := snap, wal := tail, garbage := s.disk.garbage ++ s.disk.wal.take (snap.boundary - s.disk.checkpoint.boundary) }, scratch := none }
  | truncate (s : State) (remaining : List Record) (h : remaining.Sublist s.disk.garbage) :
      Move s { s with disk := { s.disk with garbage := remaining } }
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
  scratch : ∀ snap, s.scratch = some snap →
    snap.image = replay {} snap.past ∧ snap.boundary = snap.past.length

theorem inv_init : Inv {} := by
  constructor <;> simp [Disk.log, PendingOK, replay]

theorem restore_eq {s : State} (h : Inv s) : s.disk.restore = replay {} s.disk.log := by
  simp [Disk.restore, h.snapshot, Disk.log, replay_append]

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
  | fsync r hp => simpa [VisibleInv, hp] using h
  | flush r hp =>
      intro hr
      have hv := h hr
      simp only [hp] at hv
      obtain ⟨prior, hl, hi⟩ := hv
      change applyRecord s.visible r = replay {} s.disk.log
      rw [hi, hl, replay_append]
      rfl
  | ack r hp => simpa [VisibleInv, hp] using h
  | checkpointStart => exact h
  | checkpointBytes => exact h
  | publish snap tail hs hp => simpa only [VisibleInv, Disk.log, ← hp] using h
  | truncate => exact h
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
  | fsync r hp => exact ⟨h.live, by simpa [PendingClock, hp] using h.pending⟩
  | flush r hp => exact ⟨h.live, by simpa [PendingClock, hp] using h.pending⟩
  | ack => exact ⟨h.live, trivial⟩
  | checkpointStart => exact ⟨h.live, h.pending⟩
  | checkpointBytes => exact h
  | publish snap tail hs hp =>
      exact ⟨by simpa only [Disk.log, ← hp] using h.live, h.pending⟩
  | truncate => exact ⟨h.live, h.pending⟩
  | crash => exact ⟨by simp, trivial⟩
  | restart wall hr hi hc =>
      refine ⟨?_, by simp [PendingClock, restartState, hi]⟩
      intro _ r hm
      have hb := replay_clock_record {} s.disk.log r hm
      have h₁ := Nat.le_max_right s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock
      have h₂ := Nat.le_max_left (max s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock) wall
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
  | bytes r hp => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.scratch⟩
  | finish r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | persist r hp =>
      have ho : ∀ c ∈ s.disk.log, c.ts < r.ts := by simpa [hp, PendingOK] using h.pending
      have hl : s.disk.checkpoint.past ++ (s.disk.wal ++ [r]) = s.disk.log ++ [r] :=
        (List.append_assoc _ _ _).symm
      refine ⟨h.snapshot, h.boundary, ?_, ?_, ?_, h.scratch⟩
      · intro c hc
        change c ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; exact List.mem_append_left _ (h.acked c hc)
      · change (s.disk.checkpoint.past ++ (s.disk.wal ++ [r])).Pairwise _
        rw [hl, List.pairwise_append]
        exact ⟨h.ordered, by simp, by simpa using ho⟩
      · change r ∈ s.disk.checkpoint.past ++ (s.disk.wal ++ [r])
        rw [hl]; simp
  | fsync r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | flush r hp => exact { h with pending := by simpa [hp, PendingOK] using h.pending }
  | ack r hp =>
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
  | checkpointBytes => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.scratch⟩
  | publish snap tail hs hp =>
      obtain ⟨hi, hb⟩ := h.scratch snap hs
      refine ⟨hi, hb, ?_, ?_, ?_, ?_⟩
      · simpa only [Disk.log, ← hp] using h.acked
      · simpa only [Disk.log, ← hp] using h.ordered
      · simpa only [Disk.log, ← hp] using h.pending
      · simp
  | truncate => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.scratch⟩
  | crash => exact { h with pending := trivial, scratch := by simp }
  | restart => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.scratch⟩
  | corruption => exact ⟨h.snapshot, h.boundary, h.acked, h.ordered, h.pending, h.scratch⟩

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

/-- The current append stream includes a complete but unpersisted final
record. Crash chooses its durable prefix; partial bytes are not records. -/
def appendStream (s : State) : List Record :=
  match s.pending with
  | .complete r => s.disk.log ++ [r]
  | _ => s.disk.log

theorem recovery_prefix (s : State) : s.disk.log.IsPrefix (appendStream s) := by
  cases hp : s.pending <;> simp [appendStream, hp]

/-- (c) Includes partial snapshot writes, publication, deletion and crashes. -/
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

/-- The checkpoint also carries clock reservations above committed timestamps. -/
theorem checkpoint_clock_restart (s : State) (hr : Reachable s) (wall ts : Nat)
    (h : max (max s.disk.checkpoint.clockHigh s.disk.restore.clock) wall + 1 ≤ ts) :
    s.disk.checkpoint.clockHigh < ts ∧ wall < ts ∧ ∀ r ∈ s.disk.log, r.ts < ts := by
  rw [restore_eq (reachable_inv hr)] at h
  have h₁ := Nat.le_max_left s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock
  have h₂ := Nat.le_max_right s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock
  have h₃ := Nat.le_max_left (max s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock) wall
  have h₄ := Nat.le_max_right (max s.disk.checkpoint.clockHigh (replay {} s.disk.log).clock) wall
  refine ⟨by omega, by omega, ?_⟩
  intro r hr
  have := replay_clock_record {} s.disk.log r hr
  omega

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

/-- (d) for EVERY later reservation in the recovered epoch, not just its first
commit. The final premise is precisely Move.begin's clock reservation rule. -/
theorem every_post_restart_timestamp (before : State) (hr : Reachable before)
    (wall : Nat) {s : State} (he : LiveEpoch (restartState before wall) s)
    (ts : Nat) (hc : s.clock < ts) :
    before.disk.checkpoint.clockHigh < ts ∧ wall < ts ∧
      ∀ r ∈ before.disk.log, r.ts < ts := by
  apply checkpoint_clock_restart before hr wall ts
  have h := (epoch_clock he).1
  change max (max before.disk.checkpoint.clockHigh before.disk.restore.clock) wall + 1 ≤ s.clock at h
  omega

/-- Instance DROP also removes every database owned by that incarnation. -/
theorem instance_drop_survives (prior tail : List Record) (inst db ts : Nat)
    (ho : (replay {} prior).owner db = inst) (hn : ∀ r ∈ tail, NoCreate db r) :
    (replay {} (prior ++ ⟨ts, .dropInstance inst⟩ :: tail)).databases db = none := by
  rw [replay_append]
  apply dropped_stays_dropped _ db tail
  · simp [applyRecord, applyEffect, ho]
  · exact hn

end TxnSpec.Persistence
