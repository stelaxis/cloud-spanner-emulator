import TxnSpec.Serial
import TxnSpec.Target

/-!
# Parallelism: disjoint transactions never abort each other

Two RW transactions whose read sets and write sets do not intersect both run
to the end without `ABORTED`, under every interleaving. The sets are the
static, data-independent ones: every key set an operation names (`fp`, what
validation checks) against every key set it may write (`writeSpans`).

With `pushdown = false` an SQL statement's read set is its whole table, so two
transactions that both run SQL against one table and one of them writes it are
*not* disjoint; with `pushdown = true` only the predicates' key sets count.
-/

namespace TxnSpec.Target

def stepsOf (σ : List Step) (t : Tid) : List Step := σ.filter fun st => st.txn == t

/-- Every key set `t`'s operations in `σ` read or validate. -/
def staticReads (cfg : Cfg) (σ : List Step) (t : Tid) : List Span :=
  (stepsOf σ t).flatMap fun st => fp cfg st.op

/-- Every key set `t`'s operations in `σ` may write. -/
def staticWrites (σ : List Step) (t : Tid) : List Span :=
  (stepsOf σ t).flatMap fun st => writeSpans st.op

def Disjoint (A B : List Span) : Prop := ∀ rk, covered A rk = true → covered B rk = false

/-! ## Where buffered writes come from -/

theorem put_buf (l : Local) (rk : RowKey) (v : Val) (w : RowKey × Option Val) :
    w ∈ (l.put rk v).buf → w ∈ l.buf ∨ w.1 = rk := by
  simp only [Local.put, List.mem_cons]
  rintro (rfl | h)
  · exact Or.inr rfl
  · exact Or.inl h

theorem del_buf (l : Local) (rk : RowKey) (w : RowKey × Option Val) :
    w ∈ (l.del rk).buf → w ∈ l.buf ∨ w.1 = rk := by
  simp only [Local.del, List.mem_cons]
  rintro (rfl | h)
  · exact Or.inr rfl
  · exact Or.inl h

theorem putAll_buf (tbl : Nat) (v : Val) (l : Local) (ks : List Key) (w : RowKey × Option Val) :
    w ∈ (putAll tbl v l ks).buf → w ∈ l.buf ∨ (w.1.tbl = tbl ∧ w.1.key ∈ ks) := by
  induction ks generalizing l with
  | nil => exact Or.inl
  | cons k ks ih =>
    intro h
    rcases ih _ h with h' | ⟨ht, hk⟩
    · rcases put_buf l _ v w h' with h'' | e
      · exact Or.inl h''
      · exact Or.inr ⟨by rw [e], by rw [e]; exact List.mem_cons_self⟩
    · exact Or.inr ⟨ht, List.mem_cons_of_mem k hk⟩

theorem delAll_buf (tbl : Nat) (l : Local) (ks : List Key) (w : RowKey × Option Val) :
    w ∈ (delAll tbl l ks).buf → w ∈ l.buf ∨ (w.1.tbl = tbl ∧ w.1.key ∈ ks) := by
  induction ks generalizing l with
  | nil => exact Or.inl
  | cons k ks ih =>
    intro h
    rcases ih _ h with h' | ⟨ht, hk⟩
    · rcases del_buf l _ w h' with h'' | e
      · exact Or.inl h''
      · exact Or.inr ⟨by rw [e], by rw [e]; exact List.mem_cons_self⟩
    · exact Or.inr ⟨ht, List.mem_cons_of_mem k hk⟩

theorem matched_contains {V : View} {tbl : Nat} {spec : KeySpec} {k : Key}
    (h : k ∈ matched V tbl spec) : spec.contains k = true := by
  simp only [matched, List.mem_filter, Bool.and_eq_true] at h
  exact h.2.1

theorem covers_of {tbl : Nat} {spec : KeySpec} {rk : RowKey} (ht : rk.tbl = tbl)
    (hk : spec.contains rk.key = true) : covered [⟨tbl, spec⟩] rk = true := by
  simp [covered, Span.covers, ht, hk]

theorem applyMut_buf {V : View} {l l' : Local} {m : Mut} (h : applyMut V l m = .ok l')
    (w : RowKey × Option Val) (hw : w ∈ l'.buf) : w ∈ l.buf ∨ covered [m.span] w.1 = true := by
  have hc : ∀ w : RowKey × Option Val, w.1 = m.rk → covered [m.span] w.1 = true := by
    intro w e; rw [e]; simp [covered, Span.covers, Mut.span, Mut.rk, KeySpec.contains]
  unfold applyMut at h
  cases hk : m.kind <;> simp only [hk] at h
  · by_cases h₁ : (l.view V m.rk).isSome = true <;> simp [h₁] at h
    subst h; rcases put_buf l _ _ w hw with h' | e
    · exact Or.inl h'
    · exact Or.inr (hc w e)
  · by_cases h₁ : m.rk ∈ l.deleted
    · simp [h₁] at h
    · by_cases h₂ : (l.view V m.rk).isNone = true <;> simp [h₁, h₂] at h
      subst h; rcases put_buf l _ _ w hw with h' | e
      · exact Or.inl h'
      · exact Or.inr (hc w e)
  · simp at h; subst h; rcases put_buf l _ _ w hw with h' | e
    · exact Or.inl h'
    · exact Or.inr (hc w e)
  · by_cases h₁ : (l.view V m.rk).isSome = true <;> simp [h₁] at h <;> subst h
    · rcases del_buf l _ w hw with h' | e
      · exact Or.inl h'
      · exact Or.inr (hc w e)
    · exact Or.inl hw

theorem applyMuts_buf {V : View} {l l' : Local} {ms : List Mut} (h : applyMuts V l ms = .ok l')
    (w : RowKey × Option Val) (hw : w ∈ l'.buf) :
    w ∈ l.buf ∨ covered (ms.map Mut.span) w.1 = true := by
  induction ms generalizing l with
  | nil => simp only [applyMuts, Except.ok.injEq] at h; subst h; exact Or.inl hw
  | cons m ms ih =>
    simp only [applyMuts] at h
    split at h
    · rename_i l₁ h₁
      rcases ih h with h' | h'
      · rcases applyMut_buf h₁ w h' with h'' | h''
        · exact Or.inl h''
        · simp only [covered, List.any_cons, List.any_nil, Bool.or_false] at h''
          exact Or.inr (by simp [covered, h''])
      · simp only [covered] at h'
        exact Or.inr (by simp [covered, h'])
    · cases h

theorem step_buf {V : View} {l l' : Local} {op : Op} {r : Res} (h : Local.step V l op = .ok (r, l'))
    (w : RowKey × Option Val) (hw : w ∈ l'.buf) : w ∈ l.buf ∨ covered (writeSpans op) w.1 = true := by
  cases op <;> simp only [Local.step] at h
  case read => simp only [Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨-, rfl⟩ := h; exact Or.inl hw
  case sql => simp only [Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨-, rfl⟩ := h; exact Or.inl hw
  case dmlInsert tbl k v =>
    split at h
    · rename_i l₁ h₁
      simp only [Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨-, rfl⟩ := h
      rcases applyMut_buf h₁ w hw with h' | h'
      · exact Or.inl h'
      · exact Or.inr (by simpa [writeSpans, Mut.span] using h')
    · cases h
  case dmlUpdate tbl spec v =>
    simp only [Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨-, rfl⟩ := h
    rcases putAll_buf tbl v l _ w hw with h' | ⟨ht, hk⟩
    · exact Or.inl h'
    · exact Or.inr (covers_of ht (matched_contains hk))
  case dmlDelete tbl spec =>
    simp only [Except.ok.injEq, Prod.mk.injEq] at h; obtain ⟨-, rfl⟩ := h
    rcases delAll_buf tbl l _ w hw with h' | ⟨ht, hk⟩
    · exact Or.inl h'
    · exact Or.inr (covers_of ht (matched_contains hk))
  all_goals cases h

theorem inactiveStep_fields (x : TxnSt) (op : Op) :
    (inactiveStep x op).2.rset = x.rset ∧ (inactiveStep x op).2.loc = x.loc ∧
      (inactiveStep x op).2.snap = x.snap ∧ (inactiveStep x op).2.kind = x.kind := by
  unfold inactiveStep; split <;> simp

/-! ## The run invariant -/

section
variable (cfg : Cfg) (σ : List Step) (t₁ t₂ : Tid) (base : Nat)

def Ours (t : Tid) : Prop := t = t₁ ∨ t = t₂

/-- While only `t₁` and `t₂` run from a state whose log had `base` commits. -/
structure J (s : State) : Prop where
  notAborted : ∀ t, Ours t₁ t₂ t → (s.txns t).status ≠ .dead .aborted
  rset : ∀ t, Ours t₁ t₂ t → ∀ sp ∈ (s.txns t).rset, sp ∈ staticReads cfg σ t
  buf : ∀ t, Ours t₁ t₂ t → ∀ w ∈ (s.txns t).loc.buf, covered (staticWrites σ t) w.1 = true
  snap : ∀ t, Ours t₁ t₂ t → ((s.txns t).kind = .rw ∨ (s.txns t).status = .idle) →
    ∀ n, (s.txns t).snap = some n → base ≤ n
  logBase : base ≤ s.log.length
  commits : ∀ c ∈ s.log.drop base, Ours t₁ t₂ c.tid ∧
    (∀ w ∈ c.writes, covered (staticWrites σ c.tid) w.1 = true) ∧
    ∃ ts, (s.txns c.tid).status = .committed ts

end

theorem mem_drop_of_le {l : List Commit} {base n : Nat} (h : base ≤ n) {c : Commit}
    (hc : c ∈ l.drop n) : c ∈ l.drop base := by
  have e : l.drop n = (l.drop base).drop (n - base) := by
    rw [List.drop_drop]; congr 1; omega
  rw [e] at hc
  exact List.mem_of_mem_drop hc

/-- Validation never fails for one of the pair: every later commit is the
other's, and it writes nothing this one reads. -/
theorem not_stale {cfg : Cfg} {σ : List Step} {t₁ t₂ : Tid} {base : Nat} {s : State}
    (hJ : J cfg σ t₁ t₂ base s)
    (disj : ∀ t u, Ours t₁ t₂ t → Ours t₁ t₂ u → t ≠ u →
      Disjoint (staticReads cfg σ t) (staticWrites σ u))
    {t : Tid} (ht : Ours t₁ t₂ t) (hnc : ∀ ts, (s.txns t).status ≠ .committed ts)
    {n : Nat} (hn : base ≤ n) {S : List Span} (hS : ∀ sp ∈ S, sp ∈ staticReads cfg σ t) :
    stale s.log n S = false := by
  unfold stale
  rw [List.any_eq_false]
  intro c hc hconf
  obtain ⟨hco, hw, ts, hst⟩ := hJ.commits c (mem_drop_of_le hn hc)
  have ne : t ≠ c.tid := fun e => by subst e; exact hnc ts hst
  simp only [conflicts, List.any_eq_true] at hconf
  obtain ⟨w, hwm, hcov⟩ := hconf
  have h₁ := covered_mono hS hcov
  have h₂ := disj t c.tid ht hco ne w.1 h₁
  rw [hw w hwm] at h₂
  cases h₂

theorem step_J (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step) (t₁ t₂ : Tid)
    (base : Nat)
    (disj : ∀ t u, Ours t₁ t₂ t → Ours t₁ t₂ u → t ≠ u →
      Disjoint (staticReads cfg σ t) (staticWrites σ u))
    (s : State) (hJ : J cfg σ t₁ t₂ base s) (st : Step) (hours : Ours t₁ t₂ st.txn)
    (hR : ∀ sp ∈ fp cfg st.op, sp ∈ staticReads cfg σ st.txn)
    (hW : ∀ sp ∈ writeSpans st.op, sp ∈ staticWrites σ st.txn) :
    J cfg σ t₁ t₂ base (step cfg s st).2 ∧ (step cfg s st).1 ≠ .err .aborted := by
  obtain ⟨t, op⟩ := st
  simp only at hours hR hW ⊢
  -- Replacing `t`'s state, log unchanged.
  have keep : ∀ x' : TxnSt, x'.status ≠ .dead .aborted →
      (∀ sp ∈ x'.rset, sp ∈ staticReads cfg σ t) →
      (∀ w ∈ x'.loc.buf, covered (staticWrites σ t) w.1 = true) →
      ((x'.kind = .rw ∨ x'.status = .idle) → ∀ n, x'.snap = some n → base ≤ n) →
      ((∃ ts, (s.txns t).status = .committed ts) → ∃ ts, x'.status = .committed ts) →
      J cfg σ t₁ t₂ base (s.set t x') := by
    intro x' ha hr hb hs hc
    refine ⟨fun t' ho => ?_, fun t' ho => ?_, fun t' ho => ?_, fun t' ho => ?_, hJ.logBase,
      fun c hc' => ?_⟩
    · by_cases e : t' = t
      · subst e; simpa [State.set] using ha
      · simpa [State.set, e] using hJ.notAborted t' ho
    · by_cases e : t' = t
      · subst e; simpa [State.set] using hr
      · simpa [State.set, e] using hJ.rset t' ho
    · by_cases e : t' = t
      · subst e; simpa [State.set] using hb
      · simpa [State.set, e] using hJ.buf t' ho
    · by_cases e : t' = t
      · subst e; simpa [State.set] using hs
      · simpa [State.set, e] using hJ.snap t' ho
    · obtain ⟨hco, hw, hst⟩ := hJ.commits c hc'
      refine ⟨hco, hw, ?_⟩
      by_cases e : c.tid = t
      · simp only [State.set, e, upd_same]; rw [e] at hst; exact hc hst
      · simpa [State.set, e] using hst
  have ha₀ := hJ.notAborted t hours
  have hr₀ := hJ.rset t hours
  have hb₀ := hJ.buf t hours
  have hs₀ := hJ.snap t hours
  unfold step
  simp only
  generalize hxs : s.txns t = x at ha₀ hr₀ hb₀ hs₀
  split
  · -- idle: begin
    rename_i hst
    have hlen : base ≤ s.log.length := hJ.logBase
    have hnc : ¬ ∃ ts, x.status = .committed ts := fun ⟨ts, e⟩ => by simp [hst] at e
    have hs₁ : ∀ n, x.snap = some n → base ≤ n := hs₀ (Or.inr hst)
    cases op with
    | beginRW =>
      by_cases hsab : cfg.snapAtBegin = true
      · simp only [beginStep, hsab, Bool.true_and, beq_self_eq_true, ite_true]
        refine ⟨keep _ (by simp) hr₀ hb₀ ?_ (fun h => absurd h (by rw [hxs]; exact hnc)), by simp⟩
        intro _ n e; simp only [Option.some.injEq] at e; omega
      · simp only [beginStep, hsab, Bool.false_and, Bool.false_eq_true, ite_false]
        exact ⟨keep _ (by simp) hr₀ hb₀ (fun _ => hs₁) (fun h => absurd h (by rw [hxs]; exact hnc)),
          by simp⟩
    | beginRO a =>
      simp only [beginStep, Bool.and_eq_true, beq_iff_eq, reduceCtorEq, and_false, ite_false]
      refine ⟨keep _ (by simp) hr₀ hb₀ (by simp) (fun h => absurd h (by rw [hxs]; exact hnc)), by simp⟩
    | _ =>
      simp only [beginStep, Bool.and_eq_true, beq_iff_eq, reduceCtorEq, and_false, ite_false]
      refine ⟨keep _ (by simp [hst]) hr₀ hb₀ (fun _ => hs₁) (fun h => absurd h (by rw [hxs]; exact hnc)), by simp⟩
  · rename_i hst
    split
    · -- RO
      unfold roStep
      split
      · rename_i r hr
        refine ⟨keep _ (by simp_all) hr₀ hb₀ hs₀ (by simp_all), ?_⟩
        intro e; subst e
        revert hr; generalize stateAt s.log (x.snap.getD 0) = V
        cases op <;> simp [roRead]
      · exact ⟨keep _ ha₀ hr₀ hb₀ hs₀ (fun ⟨ts, e⟩ => by simp_all), by simp⟩
    · -- RW
      rename_i hkind
      have hnc : ∀ ts, (s.txns t).status ≠ .committed ts := by simp [hxs, hst]
      have hn : base ≤ x.snap.getD s.log.length := by
        cases e : x.snap with
        | none => exact hJ.logBase
        | some n => exact hs₀ (Or.inl hkind) n e
      have hns : ∀ S, (∀ sp ∈ S, sp ∈ staticReads cfg σ t) → stale s.log (x.snap.getD s.log.length) S = false :=
        fun S hS => not_stale hJ disj hours hnc hn hS
      unfold rwStep
      dsimp only
      split
      · -- commit
        rename_i ms
        have hS : ∀ sp ∈ x.rset ++ fp cfg (.commit ms), sp ∈ staticReads cfg σ t := by
          intro sp hsp
          rcases List.mem_append.mp hsp with h | h
          · exact hr₀ sp h
          · exact hR sp h
        rw [hns _ hS]
        simp only [Bool.false_eq_true, ite_false]
        split
        · rename_i l' hm
          -- the new commit writes only what `t` may write
          have hwc : ∀ w ∈ l'.buf, covered (staticWrites σ t) w.1 = true := by
            intro w hw
            rcases applyMuts_buf hm w hw with h | h
            · exact hb₀ w h
            · exact covered_mono (fun sp hsp => hW sp (by simpa [writeSpans] using hsp)) h
          refine ⟨⟨fun t' ho => ?_, fun t' ho => ?_, fun t' ho => ?_, fun t' ho => ?_,
            by have := hJ.logBase; simp; omega, fun c hc => ?_⟩, by simp⟩
          · by_cases e : t' = t
            · subst e; simp
            · simpa [e] using hJ.notAborted t' ho
          · by_cases e : t' = t
            · subst e; simpa using hS
            · simpa [e] using hJ.rset t' ho
          · by_cases e : t' = t
            · subst e; simpa using hwc
            · simpa [e] using hJ.buf t' ho
          · by_cases e : t' = t
            · subst e; simp only [upd_same, Option.some.injEq]; intro _ n e; omega
            · simpa [e] using hJ.snap t' ho
          · rw [List.drop_append_of_le_length hJ.logBase] at hc
            rcases List.mem_append.mp hc with hc | hc
            · obtain ⟨hco, hw, ts, hst'⟩ := hJ.commits c hc
              have ne : c.tid ≠ t := fun e => by rw [e] at hst'; exact hnc ts hst'
              exact ⟨hco, hw, ts, by simpa [ne] using hst'⟩
            · simp only [List.mem_singleton] at hc
              subst hc
              exact ⟨hours, fun w hw => hwc w hw, s.log.length + 1, by simp⟩
        · rename_i c hm
          have hc : c ≠ .aborted := applyMuts_not_aborted hm
          exact ⟨keep _ (by simpa using hc) hr₀ hb₀ (fun _ => hs₀ (Or.inl hkind)) (fun ⟨ts, e⟩ => absurd e (hnc ts)),
            by simpa using hc⟩
      · -- rollback
        exact ⟨keep _ (by simp) hr₀ hb₀ (fun _ => hs₀ (Or.inl hkind)) (fun ⟨ts, e⟩ => absurd e (hnc ts)), by simp⟩
      · -- data op
        split
        · rename_i hd
          split
          · rename_i r l' hs'
            refine ⟨keep _ (by simp_all) ?_ ?_ ?_ (fun ⟨ts, e⟩ => absurd e (hnc ts)),
              step_ok_res hs' .aborted⟩
            · intro sp hsp
              simp only [List.mem_append, rsetOf, hk, Bool.false_eq_true, ite_false] at hsp
              rcases hsp with h | h
              · exact hr₀ sp h
              · exact hR sp h
            · intro w hw
              rcases step_buf hs' w hw with h | h
              · exact hb₀ w h
              · exact covered_mono hW h
            · intro _ n e; simp only [Option.some.injEq] at e; omega
          · rename_i c hs'
            have hS : ∀ sp ∈ x.rset ++ fp cfg op, sp ∈ staticReads cfg σ t := by
              intro sp hsp
              rcases List.mem_append.mp hsp with h | h
              · exact hr₀ sp h
              · exact hR sp h
            have he : errCode cfg s.log (x.snap.getD s.log.length) (x.rset ++ fp cfg op) c = c := by
              simp [errCode, hns _ hS]
            have hc : c ≠ .aborted := step_not_aborted hs' hd
            rw [he]
            exact ⟨keep _ (by simpa using hc) hr₀ hb₀ (fun _ => hs₀ (Or.inl hkind)) (fun ⟨ts, e⟩ => absurd e (hnc ts)),
              by simpa using hc⟩
        · exact ⟨by simpa [State.set] using hJ, by simp⟩
  · -- dead / committed / rolled back
    rename_i hi ha
    have hres : (inactiveStep x op).1 ≠ .err .aborted := by
      unfold inactiveStep
      split <;> simp_all
    obtain ⟨fr, fl, fs, fk⟩ := inactiveStep_fields x op
    refine ⟨keep _ ?_ (by rw [fr]; exact hr₀) (by rw [fl]; exact hb₀) ?_ ?_, hres⟩
    · unfold inactiveStep; split <;> simp_all
    · rw [fs, fk]
      intro h
      apply hs₀
      rcases h with h | h
      · exact Or.inl h
      · exfalso; revert h; unfold inactiveStep; split <;> simp_all
    · rintro ⟨ts, e⟩
      rw [hxs] at e
      unfold inactiveStep; split <;> simp_all

theorem run_cons_fst (cfg : Cfg) (s : State) (st : Step) (sts : List Step) :
    (run cfg s (st :: sts)).1 = (step cfg s st).1 :: (run cfg (step cfg s st).2 sts).1 := by
  simp only [run]

theorem run_J (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (σ : List Step) (t₁ t₂ : Tid)
    (base : Nat)
    (disj : ∀ t u, Ours t₁ t₂ t → Ours t₁ t₂ u → t ≠ u →
      Disjoint (staticReads cfg σ t) (staticWrites σ u))
    (τ : List Step) (hτ : ∀ st ∈ τ, Ours t₁ t₂ st.txn ∧
      (∀ sp ∈ fp cfg st.op, sp ∈ staticReads cfg σ st.txn) ∧
      (∀ sp ∈ writeSpans st.op, sp ∈ staticWrites σ st.txn)) :
    ∀ s, J cfg σ t₁ t₂ base s → Res.err .aborted ∉ (run cfg s τ).1 := by
  induction τ with
  | nil => intro s _; simp [run]
  | cons st sts ih =>
    intro s hJ
    obtain ⟨ho, hR, hW⟩ := hτ st List.mem_cons_self
    obtain ⟨hJ', hr⟩ := step_J cfg hk σ t₁ t₂ base disj s hJ st ho hR hW
    rw [run_cons_fst, List.mem_cons, not_or]
    exact ⟨fun e => hr e.symm,
      ih (fun st' h => hτ st' (List.mem_cons_of_mem st h)) _ hJ'⟩

/-- **(iii) Parallelism.** Two fresh RW transactions `t₁ ≠ t₂`, run in any
interleaving from any state, where neither's read/validated key sets meet the
other's written key sets: no operation of either returns `ABORTED`. Each one
commits unless its own operations raise a constraint error. -/
theorem disjoint_never_abort (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false) (s : State)
    (t₁ t₂ : Tid) (σ : List Step) (hne : t₁ ≠ t₂)
    (h₁ : s.txns t₁ = {}) (h₂ : s.txns t₂ = {})
    (honly : ∀ st ∈ σ, st.txn = t₁ ∨ st.txn = t₂)
    (d₁₂ : Disjoint (staticReads cfg σ t₁) (staticWrites σ t₂))
    (d₂₁ : Disjoint (staticReads cfg σ t₂) (staticWrites σ t₁)) :
    Res.err .aborted ∉ (run cfg s σ).1 := by
  have disj : ∀ t u, Ours t₁ t₂ t → Ours t₁ t₂ u → t ≠ u →
      Disjoint (staticReads cfg σ t) (staticWrites σ u) := by
    rintro t u (rfl | rfl) (rfl | rfl) ne
    · exact absurd rfl ne
    · exact d₁₂
    · exact d₂₁
    · exact absurd rfl ne
  have hmem : ∀ st ∈ σ, (∀ sp ∈ fp cfg st.op, sp ∈ staticReads cfg σ st.txn) ∧
      (∀ sp ∈ writeSpans st.op, sp ∈ staticWrites σ st.txn) := by
    intro st hst
    have hin : st ∈ stepsOf σ st.txn := by simp [stepsOf, hst]
    exact ⟨fun sp h => List.mem_flatMap.mpr ⟨st, hin, h⟩,
      fun sp h => List.mem_flatMap.mpr ⟨st, hin, h⟩⟩
  apply run_J cfg hk σ t₁ t₂ s.log.length disj σ (fun st h => ⟨honly st h, hmem st h⟩)
  refine ⟨fun t ho => ?_, fun t ho => ?_, fun t ho => ?_, fun t ho => ?_, Nat.le_refl _, ?_⟩
  all_goals first
    | (rcases ho with rfl | rfl <;> simp [h₁, h₂])
    | (intro c hc; simp at hc)

end TxnSpec.Target
