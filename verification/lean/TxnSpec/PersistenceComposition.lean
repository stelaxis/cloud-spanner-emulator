import TxnSpec.Persistence

namespace TxnSpec.Persistence

def recoverTarget (s : Target.State) : Target.State := { log := s.log }

theorem recover_target_inv (cfg : Cfg) (s : Target.State) (h : Target.Inv cfg s) :
    Target.Inv cfg (recoverTarget s) := by
  refine ⟨h.serial, h.ts, ?_⟩
  intro t
  exact ⟨rfl, rfl, rfl, rfl, rfl⟩

/-- Specification of serial Target histories separated by transaction loss.
The storage product below derives this projection; this relation alone is not
claimed to establish persistence. -/
inductive AcrossRestarts (cfg : Cfg) : Target.State → Prop where
  | initial : AcrossRestarts cfg {}
  | call {s : Target.State} (st : Step) :
      AcrossRestarts cfg s → AcrossRestarts cfg (Target.step cfg s st).2
  | crash {s : Target.State} : AcrossRestarts cfg s → AcrossRestarts cfg (recoverTarget s)

theorem across_restarts_inv (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false)
    {s : Target.State} (h : AcrossRestarts cfg s) : Target.Inv cfg s := by
  induction h with
  | initial => exact Target.inv_init cfg
  | call st _ ih => exact Target.step_inv cfg hk _ st ih
  | crash _ ih => exact recover_target_inv cfg _ ih

/-- Complete one append using the fsync path (as opposed to early writeback). -/
def writeRecord (s : State) (r : Record) : State :=
  { s with disk := { s.disk with wal := s.disk.wal ++ [r], physical := s.disk.physical ++ [⟨s.disk.log.length, r⟩], lease := max s.disk.lease r.ts, tornTail := false }, visible := applyRecord s.visible r, clock := r.ts, acks := s.acks ++ [r] }

theorem writeRecord_reachable {s : State} (hs : Reachable s) (r : Record)
    (hi : s.pending = .idle) (hr : s.running = true) (hc : s.clock < r.ts) :
    Reachable (writeRecord s r) := by
  have h₀ := Reachable.next hs (Move.lease s (max s.disk.lease r.ts) (Nat.le_max_left _ _))
  have h₁ := Reachable.next h₀ (Move.begin _ r hi hr hc)
  have h₂ := Reachable.next h₁ (Move.finish _ r rfl)
  have h₃ := Reachable.next h₂ (Move.fsync _ r rfl)
  have h₄ := Reachable.next h₃ (Move.flush _ r rfl)
  have h₅ := Reachable.next h₄ (Move.ack _ r rfl (Nat.le_max_right _ _))
  simpa only [writeRecord, hi, Disk.log] using h₅

def bootRecord : Record := ⟨1, .createDatabase 0 0⟩
def boot : State := writeRecord {} bootRecord

def txnRecord (c : Commit × Nat) : Record := ⟨c.2, .txn 0 c.1.writes⟩

/-- The Target log uses ordinal timestamps; each entry is paired with its
strictly increasing physical clock timestamp reserved by Move.begin. -/
structure Product where
  storage : State := boot
  target : Target.State := {}
  candidate : Target.State := {}
  entry : Commit × Nat := (⟨0, 0, [], [], [], {}⟩, 0)
  history : List (Commit × Nat) := [] -- ghost durable correspondence

def Product.recovered (s : Product) : Target.State :=
  recoverTarget (match s.storage.pending with
    | .durable _ _ => s.candidate
    | _ => s.target)

/-- Calls/validation occur only with the global gate idle. A validated commit
is staged by begin, installed into Target at flush, and acknowledged at ack.
Early writeback may survive a crash before flush; crash selects it from the
actual Persistence pending/durable state. -/
inductive ProductMove (cfg : Cfg) : Product → Product → Prop where
  | call (s : Product) (st : Step) (hi : s.storage.pending = .idle)
      (hr : s.storage.running = true) (hl : (Target.step cfg s.target st).2.log = s.target.log) :
      ProductMove cfg s { s with target := (Target.step cfg s.target st).2 }
  | begin (s : Product) (tid : Tid) (ms : List Mut) (c : Commit) (ts : Nat)
      (hi : s.storage.pending = .idle) (hr : s.storage.running = true) (hc : s.storage.clock < ts)
      (hl : (Target.step cfg s.target ⟨tid, .commit ms⟩).2.log = s.target.log ++ [c]) :
      ProductMove cfg s { s with storage := { s.storage with pending := .writing (txnRecord (c, ts)), clock := ts }, candidate := (Target.step cfg s.target ⟨tid, .commit ms⟩).2, entry := (c, ts) }
  | finish (s : Product) (hp : s.storage.pending = .writing (txnRecord s.entry)) :
      ProductMove cfg s { s with storage := { s.storage with pending := .complete (txnRecord s.entry) } }
  | persist (s : Product) (synced : Bool) (hp : s.storage.pending = .complete (txnRecord s.entry)) :
      ProductMove cfg s { s with storage := { s.storage with disk := { s.storage.disk with wal := s.storage.disk.wal ++ [txnRecord s.entry], physical := s.storage.disk.physical ++ [⟨s.storage.disk.log.length, txnRecord s.entry⟩], tornTail := false }, pending := .durable (txnRecord s.entry) synced }, history := s.history ++ [s.entry] }
  | sync (s : Product) (hp : s.storage.pending = .durable (txnRecord s.entry) false) :
      ProductMove cfg s { s with storage := { s.storage with pending := .durable (txnRecord s.entry) true } }
  | flush (s : Product) (hp : s.storage.pending = .durable (txnRecord s.entry) true) :
      ProductMove cfg s { s with storage := { s.storage with pending := .installed (txnRecord s.entry), visible := applyRecord s.storage.visible (txnRecord s.entry) }, target := s.candidate }
  | ack (s : Product) (hp : s.storage.pending = .installed (txnRecord s.entry))
      (hl : s.entry.2 ≤ s.storage.disk.lease) :
      ProductMove cfg s { s with storage := { s.storage with pending := .idle, acks := s.storage.acks ++ [txnRecord s.entry] } }
  /-- Includes torn bytes, checkpoint start/bytes/publication/truncation and
  clock leases. This case requires an actual storage step, with unchanged log,
  pending phase and liveness; it cannot smuggle a commit or crash into Target. -/
  | storage (s : Product) (t : State) (m : Move s.storage t)
      (hp : t.pending = s.storage.pending) (hl : t.disk.log = s.storage.disk.log)
      (hr : t.running = s.storage.running) :
      ProductMove cfg s { s with storage := t }
  | crash (s : Product) :
      ProductMove cfg s { s with storage := { s.storage with pending := .idle, visible := {}, scratch := none, running := false, clock := 0, restartBoundary := 0 }, target := s.recovered }
  | restart (s : Product) (wall : Nat) (hr : s.storage.running = false)
      (hi : s.storage.pending = .idle) (hc : s.storage.disk.corrupt = false) :
      ProductMove cfg s { s with storage := restartState s.storage wall }

inductive ProductReachable (cfg : Cfg) : Product → Prop where
  | initial : ProductReachable cfg {}
  | next {s t : Product} : ProductReachable cfg s → ProductMove cfg s t → ProductReachable cfg t

def FlightInv (s : Product) : Prop :=
  match s.storage.pending with
  | .writing _ | .complete _ =>
      s.candidate.log = s.target.log ++ [s.entry.1] ∧ s.target.log = s.history.map Prod.fst
  | .durable _ _ => s.candidate.log = s.history.map Prod.fst
  | _ => s.target.log = s.history.map Prod.fst

structure ProductInv (cfg : Cfg) (s : Product) : Prop where
  storage : Reachable s.storage
  target : AcrossRestarts cfg s.target
  candidate : AcrossRestarts cfg s.candidate
  history : s.storage.disk.log = bootRecord :: s.history.map txnRecord
  flight : FlightInv s

theorem product_move_inv {cfg : Cfg} {s t : Product} (h : ProductInv cfg s)
    (m : ProductMove cfg s t) : ProductInv cfg t := by
  cases m with
  | call st hi hr hl =>
      refine { h with target := .call st h.target, flight := ?_ }
      simpa [FlightInv, hi, hl] using h.flight
  | begin tid ms c ts hi hr hc hl =>
      refine { h with storage := .next h.storage (.begin _ _ hi hr hc), candidate := .call _ h.target, flight := ?_ }
      exact ⟨hl, by simpa [FlightInv, hi] using h.flight⟩
  | finish hp =>
      exact { h with storage := .next h.storage (.finish _ _ hp), flight := by simpa [FlightInv, hp] using h.flight }
  | persist synced hp =>
      have hm : Move s.storage { s.storage with disk := { s.storage.disk with wal := s.storage.disk.wal ++ [txnRecord s.entry], physical := s.storage.disk.physical ++ [⟨s.storage.disk.log.length, txnRecord s.entry⟩], tornTail := false }, pending := .durable (txnRecord s.entry) synced } := by
        cases synced
        · exact .persist _ _ hp
        · exact .fsync _ _ hp
      refine { h with storage := .next h.storage hm, history := ?_, flight := ?_ }
      · simpa [Disk.log, List.map_append, ← List.append_assoc] using
          congrArg (· ++ [txnRecord s.entry]) h.history
      · have hh := h.flight
        simp only [FlightInv, hp] at hh
        simpa [FlightInv, List.map_append] using hh.1.trans (congrArg (· ++ [s.entry.1]) hh.2)
  | sync hp => exact { h with storage := .next h.storage (.syncPersisted _ _ hp), flight := by simpa [FlightInv, hp] using h.flight }
  | flush hp => exact { h with storage := .next h.storage (.flush _ _ hp), target := h.candidate, flight := by simpa [FlightInv, hp] using h.flight }
  | ack hp hl => exact { h with storage := .next h.storage (.ack _ _ hp hl), flight := by simpa [FlightInv, hp] using h.flight }
  | storage t hm hp hl hr =>
      exact { h with storage := .next h.storage hm, history := hl.trans h.history, flight := by simpa only [FlightInv, hp] using h.flight }
  | crash =>
      refine { h with storage := .next h.storage (.crash _), target := ?_, flight := ?_ }
      · unfold Product.recovered
        cases s.storage.pending <;> first | exact AcrossRestarts.crash h.target | exact AcrossRestarts.crash h.candidate
      · have hh := h.flight
        cases hp : s.storage.pending <;> simp_all [FlightInv, Product.recovered, recoverTarget]
  | restart wall hr hi hc =>
      exact { h with storage := .next h.storage (.restart _ wall hr hi hc) }

theorem product_inv {cfg : Cfg} {s : Product} (h : ProductReachable cfg s) : ProductInv cfg s := by
  induction h with
  | initial =>
      exact ⟨writeRecord_reachable .initial bootRecord rfl rfl (by decide), .initial, .initial, rfl, rfl⟩
  | next _ m ih => exact product_move_inv ih m

/-- (g.i) The actual storage product projects to the restart specification and
Target.Inv. Every product storage state is Persistence.Reachable. -/
theorem product_target_inv {cfg : Cfg} (hk : cfg.keysOnlyReadSet = false)
    {s : Product} (h : ProductReachable cfg s) :
    Reachable s.storage ∧ AcrossRestarts cfg s.target ∧ Target.Inv cfg s.target := by
  have hi := product_inv h
  exact ⟨hi.storage, hi.target, across_restarts_inv cfg hk hi.target⟩

theorem product_recovered_log {cfg : Cfg} {s : Product} (h : ProductReachable cfg s) :
    s.recovered.log = s.history.map Prod.fst := by
  have hh := (product_inv h).flight
  cases hp : s.storage.pending <;> simp_all [FlightInv, Product.recovered, recoverTarget]

theorem replay_target_rows (s : Image) (b : Database)
    (h : s.databases 0 = some b) (cs : List (Commit × Nat)) :
    ((replay s (cs.map txnRecord)).databases 0).map Database.rows =
      some (applyLog b.rows (cs.map Prod.fst)) := by
  induction cs generalizing s b with
  | nil => simp [replay, h, applyLog]
  | cons c cs ih =>
      simp only [List.map_cons, replay, applyLog]
      apply ih (applyRecord s (txnRecord c)) { b with rows := overlay c.1.writes b.rows }
      simp [applyRecord, applyEffect, txnRecord, h]

/-- (g.ii) Recovery of the ONE physical WAL/checkpoint is exactly stateAt of
the recovered Target history, including durable unacknowledged commits. -/
theorem product_recovered_rows {cfg : Cfg} {s : Product} (h : ProductReachable cfg s) :
    (s.storage.disk.restore.databases 0).map Database.rows =
      some (stateAt s.recovered.log s.recovered.log.length) := by
  have hi := product_inv h
  rw [restore_eq (reachable_inv hi.storage), hi.history, product_recovered_log h]
  simp only [replay, List.take_length, stateAt]
  exact replay_target_rows _ {} (by simp [bootRecord, applyRecord, applyEffect]) s.history

/-- Once no durable-before-flush commit is pending (in particular after crash
or restart), recovery equals the ordinary Target projection itself. -/
theorem product_recovered_target_rows {cfg : Cfg} {s : Product}
    (h : ProductReachable cfg s) (hi : s.storage.pending = .idle) :
    (s.storage.disk.restore.databases 0).map Database.rows =
      some (stateAt s.target.log s.target.log.length) := by
  simpa [Product.recovered, hi, recoverTarget] using product_recovered_rows h

/-- The installed live image matches Target when the gate is released. -/
theorem product_visible_rows {cfg : Cfg} {s : Product} (h : ProductReachable cfg s)
    (hi : s.storage.pending = .idle) (hr : s.storage.running = true) :
    (s.storage.visible.databases 0).map Database.rows =
      some (stateAt s.target.log s.target.log.length) := by
  rw [quiescent_image (product_inv h).storage hi hr,
    ← restore_eq (reachable_inv (product_inv h).storage)]
  exact product_recovered_target_rows h hi

/-- Explicit ordinal-to-physical mapping: both lists have the same positions;
Target ordinals are 1..n and the physical timestamps are strictly increasing. -/
theorem product_timestamp_mapping {cfg : Cfg} (hk : cfg.keysOnlyReadSet = false)
    {s : Product} (h : ProductReachable cfg s) :
    TsFrom 0 s.recovered.log ∧
    s.history.Pairwise (fun a b => a.2 < b.2) ∧ s.recovered.log = s.history.map Prod.fst := by
  have hi := product_inv h
  have ha : AcrossRestarts cfg s.recovered := by
    unfold Product.recovered
    cases s.storage.pending <;> first | exact AcrossRestarts.crash hi.target | exact AcrossRestarts.crash hi.candidate
  refine ⟨(across_restarts_inv cfg hk ha).ts, ?_, product_recovered_log h⟩
  have ho := (reachable_inv hi.storage).ordered
  rw [hi.history, List.pairwise_cons] at ho
  simpa [List.pairwise_map, txnRecord] using ho.2

/-- The recovery projection includes any durable uncertain candidate and loses
all open handles. Its serial invariant follows from the product transition proof. -/
theorem product_recovered_inv {cfg : Cfg} (hk : cfg.keysOnlyReadSet = false)
    {s : Product} (h : ProductReachable cfg s) : Target.Inv cfg s.recovered := by
  have hi := product_inv h
  apply across_restarts_inv cfg hk
  unfold Product.recovered
  cases s.storage.pending <;> first
    | exact AcrossRestarts.crash hi.target
    | exact AcrossRestarts.crash hi.candidate

/-- (g) Serializable history and serial replay equality for the reachable
storage product, not an assumption of history survival. -/
theorem target_serializable_across_restarts {cfg : Cfg} (hk : cfg.keysOnlyReadSet = false)
    {s : Product} (h : ProductReachable cfg s) :
    Serializable s.target.log ∧ Serializable s.recovered.log ∧
    serialExec emptyView (s.recovered.log.map fun c => (c.prog, c.muts)) =
      .ok (s.recovered.log.map (·.results), stateAt s.recovered.log s.recovered.log.length) ∧
    (s.storage.disk.restore.databases 0).map Database.rows =
      some (stateAt s.recovered.log s.recovered.log.length) := by
  have hr := (product_recovered_inv hk h).serial
  exact ⟨(product_target_inv hk h).2.2.serial, hr, serialExec_eq hr, product_recovered_rows h⟩

end TxnSpec.Persistence
