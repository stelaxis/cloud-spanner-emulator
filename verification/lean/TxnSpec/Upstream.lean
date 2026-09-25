import TxnSpec.Mvcc

/-!
# Upstream model: one lock slot per database

The emulator (v1.5.58) with `--abort_current_transaction_probability=0`:

* `LockManager` keeps one active `LockHandle` per database
  (`backend/locking/manager.cc`, `EnqueueLock`). Any read or buffered write of a
  RW transaction enqueues a lock request (`TransactionStore::AcquireReadLock` /
  `AcquireWriteLock`); the first one takes the slot, the holder's later ones
  succeed, and anyone else's is aborted on the spot — nothing blocks.
* A `Commit` with mutations locks through the same path while buffering them;
  with none, `ReserveCommitTimestamp` takes a free slot or aborts
  (`manager.cc`, `ReserveCommitTimestamp`).
* The holder reads the latest committed state (`Storage::Read` at
  `InfiniteFuture`, `transaction_store.cc`), commits, and releases the slot
  (`ReadWriteTransaction::Commit`: reserve → flush → `MarkCommitted` →
  `UnlockAll`).
* Any error from a RW call resets the backend transaction, which releases the
  slot (`ReadWriteTransaction::GuardedCall` → `Reset` → `UnlockAll`); the
  frontend then replays that error on every later call except `Rollback`
  (`frontend/entities/transaction.cc`, `GuardedCall`).
* `BeginTransaction` takes nothing (`TransactionActivation::kInitializeOnly`).
* RO transactions take no locks and read at their timestamp.
-/

namespace TxnSpec.Upstream

structure State where
  log : List Commit := []
  txns : Tid → TxnSt := fun _ => {}
  slot : Option Tid := none

def State.set (s : State) (t : Tid) (x : TxnSt) : State := { s with txns := upd s.txns t x }

/-- May `t` take or keep the slot? -/
def free (s : State) (t : Tid) : Bool :=
  match s.slot with
  | none => true
  | some h => h == t

def latest (s : State) : View := stateAt s.log s.log.length

def rwStep (s : State) (t : Tid) (x : TxnSt) (op : Op) : Res × State :=
  match op with
  | .commit ms =>
      if free s t then
        match applyMuts (latest s) x.loc ms with
        | .ok l' =>
            let c : Commit := { tid := t, ts := s.log.length + 1, prog := x.prog,
                                results := x.results, muts := ms, final := l' }
            (.committed c.ts,
             { log := s.log ++ [c], slot := none,
               txns := upd s.txns t { x with status := .committed c.ts, loc := l' } })
        | .error c => (.err c, { s.set t { x with status := .dead c } with slot := none })
      else (.err .aborted, s.set t { x with status := .dead .aborted })
  | .rollback =>
      (.ok, { s.set t { x with status := .rolledBack } with
              slot := if free s t then none else s.slot })
  | op =>
      if op.isData then
        if free s t then
          match Local.step (latest s) x.loc op with
          | .ok (r, l') =>
              (r, { s.set t { x with loc := l', prog := x.prog ++ [op], results := x.results ++ [r] }
                    with slot := some t })
          | .error c => (.err c, { s.set t { x with status := .dead c } with slot := none })
        else (.err .aborted, s.set t { x with status := .dead .aborted })
      else (.err .failedPrecondition, s)

def step (s : State) (st : Step) : Res × State :=
  let t := st.txn
  let x := s.txns t
  match x.status with
  | .idle => let (r, x') := beginStep s.log.length x st.op; (r, s.set t x')
  | .active => match x.kind with
    | .ro => let (r, x') := roStep s.log x st.op; (r, s.set t x')
    | .rw => rwStep s t x st.op
  | _ => let (r, x') := inactiveStep x st.op; (r, s.set t x')

def run (s : State) : List Step → List Res × State
  | [] => ([], s)
  | st :: sts => let (r, s') := step s st; let (rs, s'') := run s' sts; (r :: rs, s'')

end TxnSpec.Upstream
