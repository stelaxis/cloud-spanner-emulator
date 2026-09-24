import TxnSpec.UpstreamProofs
import TxnSpec.Target

/-!
# Target admits every history Upstream commits

Run a schedule through Upstream; keep only the steps of the transactions it
committed; run that through Target. Target commits the same transactions in
the same order with the same reads and writes.

Why only the committed transactions' steps: on the unfiltered schedule the two
models may commit *different* sets — a transaction Upstream aborts for touching
the slot may commit under Target and then invalidate one Upstream committed
(`Counterexamples.target_differs_unfiltered`). Neither is "more permissive"
schedule-for-schedule; Target is at least as permissive history-for-history.

Requires the snapshot to be taken at the first data operation
(`snapAtBegin = false`); see `Counterexamples.snapAtBegin_rejects`.
-/

namespace TxnSpec

/-- Dead or rolled back: will never commit. -/
def Status.dropped : Status → Bool
  | .dead _ | .rolledBack => true
  | _ => false

theorem inactiveStep_dropped {x : TxnSt} {op : Op} (h : x.status.dropped = true) :
    (inactiveStep x op).2.status.dropped = true := by
  unfold inactiveStep
  split <;> simp_all [Status.dropped]

theorem inactiveStep_committed {x : TxnSt} {op : Op} {ts : Nat} (h : x.status = .committed ts) :
    (inactiveStep x op).2 = x ∧ (inactiveStep x op).1 = (match op with
      | .commit _ => .committed ts
      | _ => .err .failedPrecondition) := by
  unfold inactiveStep
  cases op <;> simp [h]

namespace Upstream

theorem step_other (u : State) (st : Step) (t' : Tid) (h : t' ≠ st.txn) :
    (step u st).2.txns t' = u.txns t' := by
  obtain ⟨t, op⟩ := st
  unfold step rwStep
  simp only at h ⊢
  split <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;>
    simp_all [State.set]

theorem step_log (u : State) (st : Step) :
    (step u st).2.log = u.log ∨ ∃ ts, ((step u st).2.txns st.txn).status = .committed ts := by
  obtain ⟨t, op⟩ := st
  unfold step rwStep
  simp only
  split <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;>
    simp_all [State.set]

theorem step_dropped (u : State) (st : Step) (t : Tid) (h : (u.txns t).status.dropped = true) :
    ((step u st).2.txns t).status.dropped = true := by
  by_cases e : t = st.txn
  · subst e
    have hs : (u.txns st.txn).status ≠ .idle ∧ (u.txns st.txn).status ≠ .active := by
      constructor <;> intro e <;> simp [e, Status.dropped] at h
    unfold step
    simp only
    split
    · simp_all
    · simp_all
    · simp only [State.set, upd_same]; exact inactiveStep_dropped h
  · rw [step_other u st t e]; exact h

theorem step_committed (u : State) (st : Step) (t : Tid) (ts : Nat)
    (h : (u.txns t).status = .committed ts) : ((step u st).2.txns t).status = .committed ts := by
  by_cases e : t = st.txn
  · subst e
    unfold step
    simp only
    split
    · simp_all
    · simp_all
    · simp only [State.set, upd_same]; rw [(inactiveStep_committed h).1]; exact h
  · rw [step_other u st t e]; exact h

theorem run_dropped (u : State) (σ : List Step) (t : Tid) (h : (u.txns t).status.dropped = true) :
    ((run u σ).2.txns t).status.dropped = true := by
  induction σ generalizing u with
  | nil => exact h
  | cons st sts ih => exact ih _ (step_dropped u st t h)

theorem run_committed (u : State) (σ : List Step) (t : Tid) (ts : Nat)
    (h : (u.txns t).status = .committed ts) : ((run u σ).2.txns t).status = .committed ts := by
  induction σ generalizing u with
  | nil => exact h
  | cons st sts ih => exact ih _ (step_committed u st t ts h)

end Upstream

namespace Target

theorem step_other (cfg : Cfg) (s : State) (st : Step) (t' : Tid) (h : t' ≠ st.txn) :
    (step cfg s st).2.txns t' = s.txns t' := by
  obtain ⟨t, op⟩ := st
  unfold step rwStep
  simp only at h ⊢
  split <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;> (try split) <;>
    simp_all [State.set]

end Target

/-! ## What a surviving step does -/

namespace Upstream

theorem step_data_survives {u : State} {t : Tid} {x : TxnSt} {d : Op} (hx : u.txns t = x)
    (hs : x.status = .active) (hk : x.kind = .rw) (hd : d.isData = true)
    (h : ((step u ⟨t, d⟩).2.txns t).status.dropped = false) :
    free u t = true ∧ ∃ r l', Local.step (latest u) x.loc d = .ok (r, l') ∧
      (step u ⟨t, d⟩).2.log = u.log ∧
      (step u ⟨t, d⟩).2.txns t = { x with loc := l', prog := x.prog ++ [d], results := x.results ++ [r] } := by
  have e : (step u ⟨t, d⟩) = rwStep u t x d := by simp [step, hx, hs, hk]
  rw [e] at h ⊢
  cases d <;> simp [Op.isData] at hd <;> simp only [rwStep, Op.isData, ite_true] at h ⊢
  all_goals
    split at h
    · rename_i hf
      refine ⟨hf, ?_⟩
      split at h
      · rename_i r l' hst
        simp only [hf, ite_true, hst]
        exact ⟨r, l', rfl, rfl, by simp [State.set]⟩
      · simp [State.set, Status.dropped] at h
    · simp [State.set, Status.dropped] at h

theorem step_commit_survives {u : State} {t : Tid} {x : TxnSt} {ms : List Mut} (hx : u.txns t = x)
    (hs : x.status = .active) (hk : x.kind = .rw)
    (h : ((step u ⟨t, .commit ms⟩).2.txns t).status.dropped = false) :
    free u t = true ∧ ∃ l', applyMuts (latest u) x.loc ms = .ok l' ∧
      (step u ⟨t, .commit ms⟩).2.log = u.log ++ [Commit.mk t (u.log.length + 1) x.prog x.results ms l'] ∧
      (step u ⟨t, .commit ms⟩).2.txns t =
        { x with status := .committed (u.log.length + 1), loc := l' } := by
  have e : (step u ⟨t, .commit ms⟩) = rwStep u t x (.commit ms) := by simp [step, hx, hs, hk]
  rw [e] at h ⊢
  simp only [rwStep] at h ⊢
  split at h
  · rename_i hf
    refine ⟨hf, ?_⟩
    split at h
    · rename_i l' hm
      simp only [hf, ite_true, hm]
      exact ⟨l', rfl, rfl, by simp⟩
    · simp [State.set, Status.dropped] at h
  · simp [State.set, Status.dropped] at h

end Upstream

namespace Target

theorem step_data_ok {cfg : Cfg} {s : State} {t : Tid} {y : TxnSt} {d : Op} {r : Res} {l' : Local}
    (hy : s.txns t = y) (hs : y.status = .active) (hk : y.kind = .rw) (hd : d.isData = true)
    (hst : Local.step (stateAt s.log (y.snap.getD s.log.length)) y.loc d = .ok (r, l')) :
    (step cfg s ⟨t, d⟩).2.log = s.log ∧
      (step cfg s ⟨t, d⟩).2.txns t =
        { y with snap := some (y.snap.getD s.log.length), loc := l', rset := y.rset ++ rsetOf cfg d r,
                 prog := y.prog ++ [d], results := y.results ++ [r] } := by
  have e : (step cfg s ⟨t, d⟩) = rwStep cfg s t y d := by simp [step, hy, hs, hk]
  rw [e]
  cases d <;> simp [Op.isData] at hd <;> simp only [rwStep, Op.isData, ite_true, hst] <;>
    simp [State.set]

theorem step_commit_ok {cfg : Cfg} {s : State} {t : Tid} {y : TxnSt} {ms : List Mut} {l' : Local}
    (hy : s.txns t = y) (hs : y.status = .active) (hk : y.kind = .rw)
    (hn : y.snap.getD s.log.length = s.log.length)
    (hm : applyMuts (stateAt s.log s.log.length) y.loc ms = .ok l') :
    (step cfg s ⟨t, .commit ms⟩).2.log = s.log ++ [Commit.mk t (s.log.length + 1) y.prog y.results ms l'] ∧
      ((step cfg s ⟨t, .commit ms⟩).2.txns t).status = .committed (s.log.length + 1) ∧
      ((step cfg s ⟨t, .commit ms⟩).2.txns t).loc = l' ∧
      ((step cfg s ⟨t, .commit ms⟩).2.txns t).prog = y.prog ∧
      ((step cfg s ⟨t, .commit ms⟩).2.txns t).results = y.results ∧
      ((step cfg s ⟨t, .commit ms⟩).2.txns t).kind = y.kind := by
  have e : (step cfg s ⟨t, .commit ms⟩) = rwStep cfg s t y (.commit ms) := by simp [step, hy, hs, hk]
  rw [e]
  have hns : stale s.log s.log.length (y.rset ++ fp cfg (.commit ms)) = false := by
    simp [stale]
  simp only [rwStep, hn, hns, Bool.false_eq_true, ite_false, hm]
  simp

end Target

/-- Target's copy of a transaction Upstream will commit. -/
structure RelTxn (x y : TxnSt) (len : Nat) : Prop where
  status : y.status = x.status
  kind : y.kind = x.kind
  prog : y.prog = x.prog
  results : y.results = x.results
  loc : y.loc = x.loc
  ro : x.kind = .ro → y.snap = x.snap
  idle : x.status = .idle → y.snap = none
  empty : x.status = .active → x.kind = .rw → x.prog = [] → y.snap = none
  started : x.status = .active → x.kind = .rw → x.prog ≠ [] → y.snap = some len

structure Rel (K : Tid → Bool) (u : Upstream.State) (s : Target.State) : Prop where
  log : s.log = u.log
  txn : ∀ t, K t = true → RelTxn (u.txns t) (s.txns t) s.log.length

theorem rel_init (K : Tid → Bool) : Rel K {} {} where
  log := rfl
  txn _ _ := {
    status := rfl, kind := rfl, prog := rfl, results := rfl, loc := rfl
    ro := fun _ => rfl, idle := fun _ => rfl, empty := fun _ _ _ => rfl
    started := fun _ _ h => absurd rfl h }

/-- Rebuild the relation after a step of `t`: other transactions are untouched,
and if the log grew, none of them had started. -/
theorem rel_update {K : Tid → Bool} {u u' : Upstream.State} {s s' : Target.State} {t : Tid}
    (hR : Rel K u s) (hlog : s'.log = u'.log) (hu : ∀ t', t' ≠ t → u'.txns t' = u.txns t')
    (hs : ∀ t', t' ≠ t → s'.txns t' = s.txns t')
    (hlen : s'.log.length = s.log.length ∨
      ∀ t', t' ≠ t → (u.txns t').status = .active → (u.txns t').kind = .rw → (u.txns t').prog = [])
    (ht : K t = true → RelTxn (u'.txns t) (s'.txns t) s'.log.length) : Rel K u' s' where
  log := hlog
  txn t' hk' := by
    by_cases e : t' = t
    · subst e; exact ht hk'
    · rw [hu t' e, hs t' e]
      have r := hR.txn t' hk'
      refine { r with started := fun ha hk hp => ?_ }
      rcases hlen with hl | hno
      · rw [hl]; exact r.started ha hk hp
      · exact absurd (hno t' e ha hk) hp

theorem rel_step (cfg : Cfg) (hsab : cfg.snapAtBegin = false) (K : Tid → Bool)
    (u : Upstream.State) (s : Target.State) (hu : Upstream.Inv u) (hR : Rel K u s) (st : Step)
    (hK : K st.txn = true → ((Upstream.step u st).2.txns st.txn).status.dropped = false)
    (hC : ∀ ts, ((Upstream.step u st).2.txns st.txn).status = .committed ts → K st.txn = true) :
    Rel K (Upstream.step u st).2 (if K st.txn then (Target.step cfg s st).2 else s) := by
  by_cases hk : K st.txn = true
  · simp only [hk, ite_true]
    obtain ⟨t, op⟩ := st
    simp only at hk hK hC ⊢
    have hKt := hK hk
    have r := hR.txn t hk
    have hlog := hR.log
    have hsu : ∀ t', t' ≠ t → (Upstream.step u ⟨t, op⟩).2.txns t' = u.txns t' :=
      fun t' e => Upstream.step_other u _ t' e
    have hst : ∀ t', t' ≠ t → (Target.step cfg s ⟨t, op⟩).2.txns t' = s.txns t' :=
      fun t' e => Target.step_other cfg s _ t' e
    generalize hx : u.txns t = x at r
    generalize hy : s.txns t = y at r
    have hyx : y.status = x.status := r.status
    cases hsx : x.status with
    | dead c =>
      exfalso
      have h₀ : (u.txns t).status.dropped = true := by rw [hx, hsx]; rfl
      have := Upstream.step_dropped u ⟨t, op⟩ t h₀
      simp_all
    | rolledBack =>
      exfalso
      have h₀ : (u.txns t).status.dropped = true := by rw [hx, hsx]; rfl
      have := Upstream.step_dropped u ⟨t, op⟩ t h₀
      simp_all
    | committed ts =>
      have eu : (Upstream.step u ⟨t, op⟩).2 = u.set t x := by
        simp [Upstream.step, hx, hsx, (inactiveStep_committed hsx).1]
      have ey : (Target.step cfg s ⟨t, op⟩).2 = s.set t y := by
        simp [Target.step, hy, hyx, hsx, (inactiveStep_committed (hyx.trans hsx)).1]
      apply rel_update hR _ hsu hst
      · left; rw [ey]; rfl
      · intro _; rw [eu, ey]; simpa [Upstream.State.set, Target.State.set] using r
      · rw [eu, ey]; exact hlog
    | idle =>
      have hfr : x.prog = [] := by
        have := hu.txn t; rw [hx] at this; unfold Upstream.UTxnInv at this; rw [hsx] at this
        exact this.2.2.2.1
      have eu : (Upstream.step u ⟨t, op⟩).2 = u.set t (beginStep u.log.length x op).2 := by
        simp [Upstream.step, hx, hsx]
      have ey : (Target.step cfg s ⟨t, op⟩).2 = s.set t (beginStep s.log.length y op).2 := by
        simp [Target.step, hy, hyx, hsx, hsab]
      have hl : s.log.length = u.log.length := by rw [hlog]
      apply rel_update hR _ hsu hst
      · left; rw [ey]; rfl
      · intro _
        rw [eu, ey]
        simp only [Upstream.set_txns, Target.State.set, upd_same, hl]
        have hsn := r.idle hsx
        obtain ⟨rs, rk, rp, rr, rl, rro, -, -, -⟩ := r
        cases op <;> simp only [beginStep] <;>
          exact { status := by simp_all, kind := by simp_all, prog := by simp_all,
                  results := by simp_all, loc := by simp_all, ro := by intros; simp_all,
                  idle := by intros; simp_all, empty := by intros; simp_all,
                  started := by intros; simp_all }
      · rw [eu, ey]; exact hlog
    | active =>
      have hl : s.log.length = u.log.length := by rw [hlog]
      cases hkx : x.kind with
      | ro =>
        have hky : y.kind = .ro := r.kind.trans hkx
        have eu : (Upstream.step u ⟨t, op⟩).2 = u.set t (roStep u.log x op).2 := by
          simp [Upstream.step, hx, hsx, hkx]
        have ey : (Target.step cfg s ⟨t, op⟩).2 = s.set t (roStep s.log y op).2 := by
          simp [Target.step, hy, hyx, hsx, hky]
        apply rel_update hR _ hsu hst
        · left; rw [ey]; rfl
        · intro _
          rw [eu, ey]
          simp only [Upstream.set_txns, Target.State.set, upd_same]
          have hsn := r.ro hkx
          obtain ⟨rs, rk, rp, rr, rl, -, -, -, -⟩ := r
          unfold roStep
          rw [hsn, hlog]
          split <;>
            exact { status := by simp_all, kind := by simp_all, prog := by simp_all,
                    results := by simp_all, loc := by simp_all, ro := by intros; simp_all,
                    idle := by intros; simp_all, empty := by intros; simp_all,
                    started := by intros; simp_all }
        · rw [eu, ey]; exact hlog
      | rw =>
        have hky : y.kind = .rw := r.kind.trans hkx
        have hsy : y.status = .active := hyx.trans hsx
        have hn : y.snap.getD s.log.length = s.log.length := by
          by_cases hp : x.prog = []
          · rw [r.empty hsx hkx hp]; rfl
          · rw [r.started hsx hkx hp]; rfl
        have hlat : stateAt s.log s.log.length = Upstream.latest u := by
          simp [Upstream.latest, hlog]
        by_cases hd : op.isData = true
        · -- data operation: both run it on the latest state
          obtain ⟨_, r₀, l', hs₀, eul, eut⟩ := Upstream.step_data_survives hx hsx hkx hd hKt
          have hs₁ : Local.step (stateAt s.log (y.snap.getD s.log.length)) y.loc op = .ok (r₀, l') := by
            rw [hn, hlat, r.loc]; exact hs₀
          obtain ⟨eyl, eyt⟩ := Target.step_data_ok hy hsy hky hd hs₁
          apply rel_update hR (by rw [eyl, eul]; exact hlog) hsu hst (Or.inl (by rw [eyl]))
          intro _
          rw [eut, eyt, eyl, hn]
          obtain ⟨rs, rk, rp, rr, rl, -, -, -, -⟩ := r
          exact { status := by simp_all, kind := by simp_all, prog := by simp_all,
                  results := by simp_all, loc := rfl, ro := by intros; simp_all,
                  idle := by intros; simp_all, empty := by intros; simp_all,
                  started := by intros; rfl }
        · cases op with
          | commit ms =>
            obtain ⟨hf, l', hm, eul, eut⟩ := Upstream.step_commit_survives hx hsx hkx hKt
            have hm' : applyMuts (stateAt s.log s.log.length) y.loc ms = .ok l' := by
              rw [hlat, r.loc]; exact hm
            obtain ⟨eyl, eys, eyloc, eyp, eyr, eyk⟩ := Target.step_commit_ok (cfg := cfg) hy hsy hky hn hm'
            apply rel_update hR (by simp [eyl, eul, hlog, r.prog, r.results]) hsu hst
            · -- nobody else had started: `t` could take the free slot
              right
              intro t' ne ha hk'
              by_cases hp : (u.txns t').prog = []
              · exact hp
              · have := Upstream.holds_slot hu ha hk' hp
                exfalso
                rcases (Upstream.free_iff u t).mp hf with e | e <;> rw [e] at this <;> simp_all
            · intro _
              rw [eut]
              exact { status := by rw [eys, hl], kind := by rw [eyk, hky]; exact hkx.symm,
                      prog := by rw [eyp, r.prog], results := by rw [eyr, r.results],
                      loc := eyloc, ro := by intro h; simp at h; simp_all,
                      idle := by intro h; simp at h, empty := by intro h; simp at h,
                      started := by intro h; simp at h }
          | rollback =>
            exfalso
            simp [Upstream.step, Upstream.rwStep, hx, hsx, hkx, Upstream.State.set,
              Status.dropped] at hKt
          | beginRW =>
            have eu : (Upstream.step u ⟨t, .beginRW⟩).2 = u := by
              simp [Upstream.step, Upstream.rwStep, hx, hsx, hkx, Op.isData]
            have ey : (Target.step cfg s ⟨t, .beginRW⟩).2 = s := by
              simp [Target.step, Target.rwStep, hy, hsy, hky, Op.isData]
            rw [eu, ey]; exact hR
          | beginRO a =>
            have eu : (Upstream.step u ⟨t, .beginRO a⟩).2 = u := by
              simp [Upstream.step, Upstream.rwStep, hx, hsx, hkx, Op.isData]
            have ey : (Target.step cfg s ⟨t, .beginRO a⟩).2 = s := by
              simp [Target.step, Target.rwStep, hy, hsy, hky, Op.isData]
            rw [eu, ey]; exact hR
          | _ => simp [Op.isData] at hd
  · -- Target does not see this step; Upstream cannot have committed it.
    simp only [hk, Bool.false_eq_true, ite_false]
    have hlog : (Upstream.step u st).2.log = u.log := by
      rcases Upstream.step_log u st with e | ⟨ts, e⟩
      · exact e
      · exact absurd (hC ts e) hk
    refine ⟨by rw [hlog]; exact hR.log, fun t ht => ?_⟩
    have ne : t ≠ st.txn := fun e => by subst e; exact hk ht
    rw [Upstream.step_other u st t ne]
    exact hR.txn t ht

theorem Upstream.run_cons (u : Upstream.State) (st : Step) (sts : List Step) :
    (Upstream.run u (st :: sts)).2 = (Upstream.run (Upstream.step u st).2 sts).2 := by
  simp only [Upstream.run]

theorem Target.run_cons (cfg : Cfg) (s : Target.State) (st : Step) (sts : List Step) :
    (Target.run cfg s (st :: sts)).2 = (Target.run cfg (Target.step cfg s st).2 sts).2 := by
  simp only [Target.run]

theorem rel_run (cfg : Cfg) (hsab : cfg.snapAtBegin = false) (K : Tid → Bool) (σ : List Step) :
    ∀ (u : Upstream.State) (s : Target.State), Upstream.Inv u → Rel K u s →
      (∀ t, K t = true → ((Upstream.run u σ).2.txns t).status.dropped = false) →
      (∀ t ts, ((Upstream.run u σ).2.txns t).status = .committed ts → K t = true) →
      Rel K (Upstream.run u σ).2 (Target.run cfg s (σ.filter fun st => K st.txn)).2 := by
  induction σ with
  | nil => intro u s _ hR _ _; exact hR
  | cons st sts ih =>
    intro u s hu hR hend hcom
    rw [Upstream.run_cons] at hend hcom ⊢
    have hK : K st.txn = true → ((Upstream.step u st).2.txns st.txn).status.dropped = false := by
      intro hk
      cases hd : ((Upstream.step u st).2.txns st.txn).status.dropped
      · rfl
      · have := Upstream.run_dropped _ sts st.txn hd
        rw [hend st.txn hk] at this; cases this
    have hC : ∀ ts, ((Upstream.step u st).2.txns st.txn).status = .committed ts → K st.txn = true :=
      fun ts h => hcom st.txn ts (Upstream.run_committed _ sts st.txn ts h)
    have hR' := rel_step cfg hsab K u s hu hR st hK hC
    have ih' := ih _ _ (Upstream.step_inv u st hu) hR' hend hcom
    rw [List.filter_cons]
    by_cases hk : K st.txn = true
    · simp only [hk, ite_true] at ih' ⊢
      rw [Target.run_cons]; exact ih'
    · simp only [hk, Bool.false_eq_true, ite_false] at ih' ⊢
      exact ih'

/-- The transactions Upstream committed. -/
def committedTxn (u : Upstream.State) (t : Tid) : Bool :=
  match (u.txns t).status with
  | .committed _ => true
  | _ => false

/-- **(iv-b) Target admits every committed history of Upstream.** Keep only
the steps of the transactions Upstream committed; Target, run on them, commits
exactly the same transactions in the same order, with the same reads, row
counts, mutations and writes (the logs are equal). -/
theorem target_admits_upstream (cfg : Cfg) (hsab : cfg.snapAtBegin = false) (σ : List Step) :
    let u := (Upstream.run {} σ).2
    (Target.run cfg {} (σ.filter fun st => committedTxn u st.txn)).2.log = u.log := by
  intro u
  have h := rel_run cfg hsab (committedTxn u) σ {} {} Upstream.inv_init (rel_init _)
    (fun t hk => by
      simp only [committedTxn, u] at hk ⊢
      split at hk
      · rename_i ts e; rw [e]; rfl
      · cases hk)
    (fun t ts e => by simp only [committedTxn, u, e])
  exact h.log

end TxnSpec
