import TxnSpec.Mvcc

/-!
# Target model: optimistic concurrency, SERIALIZABLE

* A RW transaction takes its snapshot timestamp at its first data operation
  (or at `Commit` if it has none) — `cfg.snapAtBegin` moves it to
  `BeginTransaction`. It reads the snapshot overlaid with its own buffer and
  records each operation's footprint (`fp`) in its read set.
* `Commit`, atomically (the per-database commit mutex): validate that no commit
  after the snapshot wrote a row covered by the read set, which includes the
  existence checks of the commit's own mutations. Stale → `ABORTED`. Otherwise
  apply the mutations, take timestamp `log.length + 1`, and append.
* With `cfg.validateErrors`, a constraint error raised against a stale snapshot
  is reported as `ABORTED` instead.
* RO transactions are unchanged from upstream.

No lock slot: nothing aborts a transaction except a failed validation.
-/

namespace TxnSpec.Target

structure State where
  log : List Commit := []
  txns : Tid → TxnSt := fun _ => {}

def State.set (s : State) (t : Tid) (x : TxnSt) : State := { s with txns := upd s.txns t x }

/-- Does any commit after snapshot `n` write a row covered by `S`? -/
def stale (log : List Commit) (n : Nat) (S : List Span) : Bool :=
  (log.drop n).any (conflicts S)

/-- Read-set entries an operation adds. `keysOnlyReadSet` records the rows
returned instead of the key set scanned. -/
def rsetOf (cfg : Cfg) (op : Op) (r : Res) : List Span :=
  if cfg.keysOnlyReadSet then
    match op, r with
    | .read tbl _, .rows rs => rs.map fun kv => ⟨tbl, .point kv.1⟩
    | .sql tbl _, .rows rs => rs.map fun kv => ⟨tbl, .point kv.1⟩
    | _, _ => fp cfg op
  else fp cfg op

/-- Report `c`, or `ABORTED` if the error may stem from a stale snapshot. -/
def errCode (cfg : Cfg) (log : List Commit) (n : Nat) (S : List Span) (c : Code) : Code :=
  if cfg.validateErrors && stale log n S then .aborted else c

def rwStep (cfg : Cfg) (s : State) (t : Tid) (x : TxnSt) (op : Op) : Res × State :=
  let n := x.snap.getD s.log.length
  match op with
  | .commit ms =>
      let S := x.rset ++ fp cfg op
      if stale s.log n S then (.err .aborted, s.set t { x with status := .dead .aborted })
      else
        match applyMuts (stateAt s.log n) x.loc ms with
        | .ok l' =>
            let c : Commit := { tid := t, ts := s.log.length + 1, prog := x.prog,
                                results := x.results, muts := ms, final := l' }
            (.committed c.ts,
             { log := s.log ++ [c],
               txns := upd s.txns t { x with status := .committed c.ts, snap := some n,
                                             loc := l', rset := S } })
        | .error c => (.err c, s.set t { x with status := .dead c })
  | .rollback => (.ok, s.set t { x with status := .rolledBack })
  | op =>
      if op.isData then
        match Local.step (stateAt s.log n) x.loc op with
        | .ok (r, l') =>
            (r, s.set t { x with snap := some n, loc := l', rset := x.rset ++ rsetOf cfg op r,
                                 prog := x.prog ++ [op], results := x.results ++ [r] })
        | .error c =>
            let c' := errCode cfg s.log n (x.rset ++ fp cfg op) c
            (.err c', s.set t { x with status := .dead c' })
      else (.err .failedPrecondition, s)

def step (cfg : Cfg) (s : State) (st : Step) : Res × State :=
  let t := st.txn
  let x := s.txns t
  match x.status with
  | .idle =>
      let (r, x') := beginStep s.log.length x st.op
      let x' := if cfg.snapAtBegin && st.op == .beginRW then { x' with snap := some s.log.length } else x'
      (r, s.set t x')
  | .active => match x.kind with
    | .ro => let (r, x') := roStep s.log x st.op; (r, s.set t x')
    | .rw => rwStep cfg s t x st.op
  | _ => let (r, x') := inactiveStep x st.op; (r, s.set t x')

def run (cfg : Cfg) (s : State) : List Step → List Res × State
  | [] => ([], s)
  | st :: sts => let (r, s') := step cfg s st; let (rs, s'') := run cfg s' sts; (r :: rs, s'')

end TxnSpec.Target
