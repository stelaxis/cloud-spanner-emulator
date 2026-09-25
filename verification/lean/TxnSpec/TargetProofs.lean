import TxnSpec.Serial
import TxnSpec.Target

/-!
# Target: serializability, timestamps, snapshot reads, error consistency

All theorems assume the read set records key sets, not returned rows
(`cfg.keysOnlyReadSet = false`); `Counterexamples.lean` shows why.
-/

namespace TxnSpec.Target

/-- A transaction nothing has happened to yet. -/
def Fresh (x : TxnSt) : Prop :=
  x.snap = none ∧ x.loc = {} ∧ x.rset = [] ∧ x.prog = [] ∧ x.results = []

/-- An active RW transaction: its read set is its program's footprint, and its
results and buffer are what its program produces on its snapshot. -/
def RWInv (cfg : Cfg) (log : List Commit) (x : TxnSt) : Prop :=
  x.rset = progFp cfg x.prog ∧
  match x.snap with
  | none => x.prog = [] ∧ x.results = [] ∧ x.loc = {}
  | some n => n ≤ log.length ∧ runProg (stateAt log n) {} x.prog = .ok (x.results, x.loc)

/-- An RO transaction's results are reads of its snapshot. -/
def ROInv (log : List Commit) (x : TxnSt) : Prop :=
  ∃ n, x.snap = some n ∧ n ≤ log.length ∧ x.prog.map (roRead (stateAt log n)) = x.results.map some

def TxnInv (cfg : Cfg) (log : List Commit) (x : TxnSt) : Prop :=
  match x.status with
  | .idle => Fresh x
  | .active => (x.kind = .rw → RWInv cfg log x) ∧ (x.kind = .ro → ROInv log x)
  | _ => True

structure Inv (cfg : Cfg) (s : State) : Prop where
  serial : Serializable s.log
  ts : TsFrom 0 s.log
  txn : ∀ t, TxnInv cfg s.log (s.txns t)

theorem inv_init (cfg : Cfg) : Inv cfg {} where
  serial := trivial
  ts := trivial
  txn := fun _ => by
    simp only [TxnInv]
    exact ⟨rfl, rfl, rfl, rfl, rfl⟩

/-! ## Structural lemmas -/

@[simp] theorem set_log (s : State) (t : Tid) (x : TxnSt) : (s.set t x).log = s.log := rfl

@[simp] theorem set_txns (s : State) (t : Tid) (x : TxnSt) : (s.set t x).txns = upd s.txns t x := rfl

theorem txnInv_final {cfg : Cfg} {log : List Commit} {x : TxnSt} (h₁ : x.status ≠ .idle)
    (h₂ : x.status ≠ .active) : TxnInv cfg log x := by
  unfold TxnInv
  split <;> simp_all

theorem txnInv_extend {cfg : Cfg} {log : List Commit} {x : TxnSt} (c : Commit)
    (h : TxnInv cfg log x) : TxnInv cfg (log ++ [c]) x := by
  unfold TxnInv at h ⊢
  split
  · simp_all
  · rename_i hs
    simp only [hs] at h
    refine ⟨fun hk => ?_, fun hk => ?_⟩
    · obtain ⟨hr, hsnap⟩ := h.1 hk
      refine ⟨hr, ?_⟩
      split
      · simp_all
      · rename_i n hn
        simp only [hn] at hsnap
        obtain ⟨hle, hrun⟩ := hsnap
        refine ⟨by simp; omega, ?_⟩
        rw [stateAt_append _ hle]; exact hrun
    · obtain ⟨n, hn, hle, hm⟩ := h.2 hk
      exact ⟨n, hn, by simp; omega, by rw [stateAt_append _ hle]; exact hm⟩
  · trivial

theorem inv_set {cfg : Cfg} {s : State} (h : Inv cfg s) (t : Tid) {x : TxnSt}
    (hx : TxnInv cfg s.log x) : Inv cfg (s.set t x) where
  serial := h.serial
  ts := h.ts
  txn t' := by
    by_cases e : t' = t
    · subst e; simpa using hx
    · simpa [e] using h.txn t'

theorem inv_commit {cfg : Cfg} {s : State} (h : Inv cfg s) (t : Tid) {x : TxnSt} {c : Commit}
    (hv : c.Valid (stateAt s.log s.log.length)) (hts : c.ts = s.log.length + 1)
    (hx : TxnInv cfg (s.log ++ [c]) x) :
    Inv cfg { log := s.log ++ [c], txns := upd s.txns t x } where
  serial := by
    rw [Serializable, serialFrom_append]
    exact ⟨h.serial, by rw [← stateAt_length]; exact hv⟩
  ts := by
    rw [tsFrom_append]
    exact ⟨h.ts, by omega⟩
  txn t' := by
    by_cases e : t' = t
    · subst e; simpa using hx
    · simpa [e] using txnInv_extend c (h.txn t')

theorem rwInv_run {cfg : Cfg} {log : List Commit} {x : TxnSt} (h : RWInv cfg log x) :
    x.snap.getD log.length ≤ log.length ∧
      runProg (stateAt log (x.snap.getD log.length)) {} x.prog = .ok (x.results, x.loc) := by
  obtain ⟨_, hs⟩ := h
  revert hs
  cases x.snap with
  | none =>
    rintro ⟨hp, hr, hl⟩
    simp [hp, hr, hl, runProg]
  | some n =>
    rintro ⟨hle, hrun⟩
    exact ⟨hle, hrun⟩

theorem roSnap_le (len : Nat) (at? : Option Nat) : roSnap len at? ≤ len := by
  cases at? <;> simp [roSnap]; omega

theorem inactiveStep_status {x : TxnSt} {op : Op} (h₁ : x.status ≠ .idle) (h₂ : x.status ≠ .active) :
    (inactiveStep x op).2.status ≠ .idle ∧ (inactiveStep x op).2.status ≠ .active := by
  unfold inactiveStep
  split <;> simp_all

/-! ## One step preserves the invariant -/

theorem step_inv (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (s : State) (st : Step)
    (h : Inv cfg s) : Inv cfg (step cfg s st).2 := by
  obtain ⟨t, op⟩ := st
  have hx := h.txn t
  unfold step
  simp only
  generalize hxs : s.txns t = x at hx
  unfold TxnInv at hx
  split
  · -- idle
    rename_i hst
    simp only [hst] at hx
    obtain ⟨hsn, hl, hrs, hp, hr⟩ := hx
    apply inv_set h
    cases op <;> cases hsab : cfg.snapAtBegin <;>
      simp [beginStep, TxnInv, RWInv, ROInv, Fresh, hst, hsn, hl, hrs, hp, hr, runProg, progFp,
        roSnap_le]
  · -- active
    rename_i hst
    simp only [hst] at hx
    split
    · -- RO
      rename_i hkind
      apply inv_set h
      obtain ⟨n, hn, hle, hm⟩ := hx.2 hkind
      unfold roStep
      split
      · rename_i r hr
        simp only [TxnInv, hst, hkind, reduceCtorEq, false_implies, true_and, forall_const]
        refine ⟨n, hn, hle, ?_⟩
        simp only [hn, Option.getD_some] at hr
        simp [List.map_append, hm, hr]
      · simp only [TxnInv, hst, hkind, reduceCtorEq, false_implies, true_and, forall_const]
        exact ⟨n, hn, hle, hm⟩
    · -- RW
      rename_i hkind
      have hrw := hx.1 hkind
      obtain ⟨hle, hrun⟩ := rwInv_run hrw
      unfold rwStep
      dsimp only
      split
      · -- commit
        rename_i ms
        split
        · exact inv_set h t (txnInv_final (by simp) (by simp))
        · rename_i hstale
          simp only [Bool.not_eq_true] at hstale
          split
          · rename_i l' hm
            apply inv_commit h t _ rfl (txnInv_final (by simp) (by simp))
            have hag := agree_of_not_stale hstale
            rw [hrw.1] at hag
            simp only [Commit.Valid]
            rw [← runTxn_congr (cfg := cfg) x.prog ms (by simpa [fp] using hag)]
            exact runTxn_of hrun hm
          · exact inv_set h t (txnInv_final (by simp) (by simp))
      · -- rollback
        exact inv_set h t (txnInv_final (by simp) (by simp))
      · -- data op
        split
        · rename_i hd
          split
          · rename_i r l' hs
            apply inv_set h
            simp only [TxnInv, hst, hkind, reduceCtorEq, false_implies, and_true, forall_const]
            refine ⟨?_, ?_⟩
            · simp [rsetOf, hk, hrw.1, progFp, List.flatMap_append]
            · exact ⟨hle, runProg_snoc hrun hs⟩
          · exact inv_set h t (txnInv_final (by simp) (by simp))
        · exact h
  · -- dead / committed / rolled back
    rename_i hi ha
    apply inv_set h
    obtain ⟨h₁, h₂⟩ := inactiveStep_status (op := op) hi ha
    exact txnInv_final h₁ h₂

theorem run_inv (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (s : State) (σ : List Step)
    (h : Inv cfg s) : Inv cfg (run cfg s σ).2 := by
  induction σ generalizing s with
  | nil => exact h
  | cons st sts ih => exact ih _ (step_inv cfg hk s st h)

/-! ## Theorems -/

/-- **(i) Target is serializable.** Every committed transaction's reads, row
counts and writes equal those of the serial execution in commit-timestamp
order, and the store equals the serial result. -/
theorem target_serializable (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step) :
    let log := (run cfg {} σ).2.log
    Serializable log ∧
      serialExec emptyView (log.map fun c => (c.prog, c.muts)) =
        .ok (log.map (·.results), stateAt log log.length) := by
  have h := run_inv cfg hk {} σ (inv_init cfg)
  exact ⟨h.serial, serialExec_eq h.serial⟩

/-- **(ii-a) Commit timestamps strictly increase**, and the `i`-th commit has
timestamp `i + 1`. -/
theorem target_ts_strict (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step) :
    let log := (run cfg {} σ).2.log
    TsFrom 0 log ∧ log.Pairwise fun a b => a.ts < b.ts := by
  have h := run_inv cfg hk {} σ (inv_init cfg)
  exact ⟨h.ts, tsFrom_strict h.ts⟩

/-- **(ii-b) Snapshot reads.** Every read of an RO transaction at timestamp
`n` returns the serial state after exactly the commits with `ts ≤ n`. -/
theorem target_ro_snapshot (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step) (t : Tid) :
    let s := (run cfg {} σ).2
    let x := s.txns t
    x.status = .active → x.kind = .ro →
      ∃ n, x.snap = some n ∧
        x.prog.map (roRead (applyLog emptyView (s.log.filter fun c => decide (c.ts ≤ n)))) =
          x.results.map some := by
  intro s x hs hk'
  have h := run_inv cfg hk {} σ (inv_init cfg)
  have hx := h.txn t
  simp only [TxnInv, x, s] at hs hk' hx ⊢
  rw [hs] at hx
  obtain ⟨n, hn, _, hm⟩ := hx.2 hk'
  exact ⟨n, hn, by rw [← stateAt_eq_filter h.ts]; exact hm⟩

theorem errors_latest_of_inv (cfg : Cfg) (hv : cfg.validateErrors = true) (s : State)
    (h : Inv cfg s) (t : Tid) (op : Op) (c : Code) (hs : (s.txns t).status = .active)
    (hkind : (s.txns t).kind = .rw) (hd : op.isData = true)
    (hres : (step cfg s ⟨t, op⟩).1 = .err c) (hc : c ≠ .aborted) :
    runProg (stateAt s.log s.log.length) {} ((s.txns t).prog ++ [op]) = .error c := by
  have hx := h.txn t
  generalize hxs : s.txns t = x at hx hs hkind ⊢
  unfold TxnInv at hx
  rw [hs] at hx
  have hrw := hx.1 hkind
  obtain ⟨hle, hrun⟩ := rwInv_run hrw
  unfold step at hres
  simp only [hxs, hs, hkind] at hres
  unfold rwStep at hres
  dsimp only at hres
  cases op <;> simp [Op.isData] at hd <;> simp only [Op.isData, ite_true] at hres
  all_goals
    split at hres
    · rename_i r₀ l₀ hs₀
      simp only [] at hres
      exact absurd hres (step_ok_res hs₀ c)
    · rename_i c₀ hs₀
      simp only [Res.err.injEq] at hres
      unfold errCode at hres
      split at hres
      · exact absurd hres.symm hc
      · rename_i hns
        simp only [hv, Bool.true_and, Bool.not_eq_true] at hns
        subst hres
        have hag := agree_of_not_stale hns
        rw [hrw.1] at hag
        apply runProg_snoc_error
        · rw [← runProg_congr (cfg := cfg) {} _ hag.left]; exact hrun
        · rw [← step_congr (cfg := cfg) _ _ hag.right]; exact hs₀

theorem commit_errors_latest_of_inv (cfg : Cfg) (s : State) (h : Inv cfg s) (t : Tid)
    (ms : List Mut) (c : Code) (hs : (s.txns t).status = .active) (hkind : (s.txns t).kind = .rw)
    (hres : (step cfg s ⟨t, .commit ms⟩).1 = .err c) (hc : c ≠ .aborted) :
    runTxn (stateAt s.log s.log.length) (s.txns t).prog ms = .error c := by
  have hx := h.txn t
  generalize hxs : s.txns t = x at hx hs hkind ⊢
  unfold TxnInv at hx
  rw [hs] at hx
  have hrw := hx.1 hkind
  obtain ⟨hle, hrun⟩ := rwInv_run hrw
  unfold step at hres
  simp only [hxs, hs, hkind] at hres
  unfold rwStep at hres
  dsimp only at hres
  split at hres
  · simp only [Res.err.injEq] at hres; exact absurd hres.symm hc
  · rename_i hns
    simp only [Bool.not_eq_true] at hns
    split at hres
    · simp at hres
    · rename_i c₀ hm
      simp only [Res.err.injEq] at hres
      subst hres
      have hag := agree_of_not_stale hns
      rw [hrw.1] at hag
      rw [← runTxn_congr (cfg := cfg) _ ms (by simpa [fp] using hag)]
      simp [runTxn, hrun, hm]

/-- **(v-a) Errors match the latest committed state.** With `validateErrors`,
a constraint error returned to a data operation is the error raised by
rerunning the transaction's program plus the failing operation on the *latest*
committed state. This is stronger than serializability: a stale-snapshot error
can be serializable (the transaction ordered before newer commits), but
clients don't retry constraint errors. `validateErrors := false` breaks this
(`Counterexamples.naive_error_not_latest`). -/
theorem target_errors_latest (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false)
    (hv : cfg.validateErrors = true) (σ : List Step) (t : Tid) (op : Op) (c : Code) :
    let s := (run cfg {} σ).2
    (s.txns t).status = .active → (s.txns t).kind = .rw → op.isData = true →
    (step cfg s ⟨t, op⟩).1 = .err c → c ≠ .aborted →
      runProg (stateAt s.log s.log.length) {} ((s.txns t).prog ++ [op]) = .error c :=
  fun hs hkind hd hres hc =>
    errors_latest_of_inv cfg hv _ (run_inv cfg hk {} σ (inv_init cfg)) t op c hs hkind hd hres hc

/-- **(v-b)** Likewise for `Commit`: a constraint error from the commit's
mutations is what running the transaction on the latest committed state
raises. Holds for every configuration, because `Commit` validates before
applying mutations. -/
theorem target_commit_errors_latest (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step)
    (t : Tid) (ms : List Mut) (c : Code) :
    let s := (run cfg {} σ).2
    (s.txns t).status = .active → (s.txns t).kind = .rw →
    (step cfg s ⟨t, .commit ms⟩).1 = .err c → c ≠ .aborted →
      runTxn (stateAt s.log s.log.length) (s.txns t).prog ms = .error c :=
  fun hs hkind hres hc =>
    commit_errors_latest_of_inv cfg _ (run_inv cfg hk {} σ (inv_init cfg)) t ms c hs hkind hres hc

end TxnSpec.Target
