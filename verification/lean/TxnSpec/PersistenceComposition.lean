import TxnSpec.Persistence

namespace TxnSpec.Persistence

/-- Recovered physical data has a ghost serial history. Open transactions and
old MVCC access are discarded; the log here is proof history, NOT an obligation
to store programs/results or preserve old snapshots on disk. -/
def recoverTarget (s : Target.State) : Target.State := { log := s.log }

theorem recover_target_inv (cfg : Cfg) (s : Target.State) (h : Target.Inv cfg s) :
    Target.Inv cfg (recoverTarget s) := by
  refine ⟨h.serial, h.ts, ?_⟩
  intro t
  exact ⟨rfl, rfl, rfl, rfl, rfl⟩

inductive AcrossRestarts (cfg : Cfg) : Target.State → Prop where
  | initial : AcrossRestarts cfg {}
  | call {s : Target.State} (st : Step) :
      AcrossRestarts cfg s → AcrossRestarts cfg (Target.step cfg s st).2
  /-- Crash before a pending commit becomes durable: its tentative validation
  and physical writes have not changed the committed Target state. -/
  | crash {s : Target.State} : AcrossRestarts cfg s → AcrossRestarts cfg (recoverTarget s)
  /-- Crash after persistence but before acknowledgement: retain the complete
  validated commit. This is a possible completion of the interrupted RPC. -/
  | crashCommit {s : Target.State} (t : Tid) (ms : List Mut) :
      AcrossRestarts cfg s →
      AcrossRestarts cfg (recoverTarget (Target.step cfg s ⟨t, .commit ms⟩).2)

theorem across_restarts_inv (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false)
    {s : Target.State} (h : AcrossRestarts cfg s) : Target.Inv cfg s := by
  induction h with
  | initial => exact Target.inv_init cfg
  | call st _ ih => exact Target.step_inv cfg hk _ st ih
  | crash _ ih => exact recover_target_inv cfg _ ih
  | crashCommit t ms _ ih =>
      exact recover_target_inv cfg _ (Target.step_inv cfg hk _ ⟨t, .commit ms⟩ ih)

/-- (g) L1 target_serializable extended to arbitrarily many restarts. The
serial history includes durable uncertain commits, but no lost open txn.
Ordinal Target timestamps may be mapped to strictly increasing physical
clock timestamps; timestamp_restart proves the restart lower bound. -/
theorem target_serializable_across_restarts (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false)
    {s : Target.State} (h : AcrossRestarts cfg s) :
    Serializable s.log ∧
      serialExec emptyView (s.log.map fun c => (c.prog, c.muts)) =
        .ok (s.log.map (·.results), stateAt s.log s.log.length) := by
  have hi := across_restarts_inv cfg hk h
  exact ⟨hi.serial, serialExec_eq hi.serial⟩

theorem open_transactions_lost (s : Target.State) (t : Tid) :
    (recoverTarget s).txns t = {} := rfl

/-- Concrete refinement bridge: resolved Target writes replay to exactly L1's
committed view. The clock is independent from its logical ordinal indices. -/
def txnRecord (db : Nat) (c : Commit) : Record := ⟨c.ts, .txn db c.writes⟩

theorem replay_target_rows (s : Image) (db : Nat) (b : Database)
    (h : s.databases db = some b) (cs : List Commit) :
    ((replay s (cs.map (txnRecord db))).databases db).map Database.rows =
      some (applyLog b.rows cs) := by
  induction cs generalizing s b with
  | nil => simp [replay, h, applyLog]
  | cons c cs ih =>
      simp only [List.map_cons, replay, applyLog]
      apply ih (applyRecord s (txnRecord db c)) { b with rows := overlay c.writes b.rows }
      simp [applyRecord, applyEffect, txnRecord, h]

end TxnSpec.Persistence
