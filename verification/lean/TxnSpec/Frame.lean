import TxnSpec.Mvcc

/-!
# Frame lemmas

An operation's result and buffer depend on the base view only through the rows
its footprint covers. This is what lets a validated snapshot stand in for the
serial state.
-/

namespace TxnSpec

theorem covered_append (A B : List Span) (rk : RowKey) :
    covered (A ++ B) rk = (covered A rk || covered B rk) := by
  simp [covered, List.any_append]

theorem covered_of_mem {S : List Span} {sp : Span} {rk : RowKey} (h : sp ∈ S)
    (hc : sp.covers rk = true) : covered S rk = true := by
  simp only [covered, List.any_eq_true]
  exact ⟨sp, h, hc⟩

theorem covered_mono {A B : List Span} (h : ∀ sp ∈ A, sp ∈ B) {rk : RowKey}
    (hc : covered A rk = true) : covered B rk = true := by
  simp only [covered, List.any_eq_true] at hc ⊢
  obtain ⟨sp, hm, hs⟩ := hc
  exact ⟨sp, h sp hm, hs⟩

theorem Agree.mono {V₁ V₂ : View} {S S' : List Span}
    (h : ∀ rk, covered S rk = true → covered S' rk = true) (ha : Agree V₁ V₂ S') :
    Agree V₁ V₂ S :=
  fun rk hc => ha rk (h rk hc)

theorem Agree.left {V₁ V₂ : View} {S S' : List Span} (ha : Agree V₁ V₂ (S ++ S')) :
    Agree V₁ V₂ S :=
  ha.mono fun rk hc => by simp [covered_append, hc]

theorem Agree.right {V₁ V₂ : View} {S S' : List Span} (ha : Agree V₁ V₂ (S ++ S')) :
    Agree V₁ V₂ S' :=
  ha.mono fun rk hc => by simp [covered_append, hc]

theorem Agree.trans {V₁ V₂ V₃ : View} {S : List Span} (h₁ : Agree V₁ V₂ S)
    (h₂ : Agree V₂ V₃ S) : Agree V₁ V₃ S :=
  fun rk hc => (h₁ rk hc).trans (h₂ rk hc)

theorem Agree.overlay {V₁ V₂ : View} {S : List Span} (ws : Writes) (h : Agree V₁ V₂ S) :
    Agree (overlay ws V₁) (overlay ws V₂) S := by
  intro rk hc
  simp only [TxnSpec.overlay, h rk hc]

theorem covers_single (tbl : Nat) (spec : KeySpec) (k : Key) :
    covered [⟨tbl, spec⟩] ⟨tbl, k⟩ = spec.contains k := by
  simp [covered, Span.covers]

/-- Agreement on a key set of one table, pointwise. -/
theorem Agree.keys {V₁ V₂ : View} {tbl : Nat} {spec : KeySpec} (h : Agree V₁ V₂ [⟨tbl, spec⟩])
    (k : Key) (hk : spec.contains k = true) : V₁ ⟨tbl, k⟩ = V₂ ⟨tbl, k⟩ :=
  h ⟨tbl, k⟩ (by rw [covers_single]; exact hk)

theorem Agree.point {V₁ V₂ : View} {tbl : Nat} {k : Key} (h : Agree V₁ V₂ [⟨tbl, .point k⟩]) :
    V₁ ⟨tbl, k⟩ = V₂ ⟨tbl, k⟩ :=
  h.keys k (by simp [KeySpec.contains])

theorem sqlSpan_covers {cfg : Cfg} {tbl : Nat} {spec : KeySpec} (rk : RowKey)
    (h : covered [⟨tbl, spec⟩] rk = true) : covered [sqlSpan cfg tbl spec] rk = true := by
  simp only [covered, List.any_cons, List.any_nil, Bool.or_false, Span.covers, sqlSpan] at h ⊢
  cases cfg.pushdown <;> simp_all [KeySpec.contains]

theorem rowsOf_congr {V₁ V₂ : View} {tbl : Nat} {spec : KeySpec}
    (h : Agree V₁ V₂ [⟨tbl, spec⟩]) : rowsOf V₁ tbl spec = rowsOf V₂ tbl spec := by
  unfold rowsOf
  congr 1
  funext k
  by_cases hk : spec.contains k = true
  · simp [hk, h.keys k hk]
  · simp [hk]

theorem matched_congr {V₁ V₂ : View} {tbl : Nat} {spec : KeySpec}
    (h : Agree V₁ V₂ [⟨tbl, spec⟩]) : matched V₁ tbl spec = matched V₂ tbl spec := by
  unfold matched
  congr 1
  funext k
  by_cases hk : spec.contains k = true
  · simp [hk, h.keys k hk]
  · simp [hk]

theorem applyMut_congr {V₁ V₂ : View} (l : Local) (m : Mut) (h : Agree V₁ V₂ [m.span]) :
    applyMut V₁ l m = applyMut V₂ l m := by
  have hp : Local.view l V₁ m.rk = Local.view l V₂ m.rk :=
    (Agree.overlay l.buf h).point
  unfold applyMut
  simp only [hp]

theorem applyMuts_congr {V₁ V₂ : View} (l : Local) (ms : List Mut)
    (h : Agree V₁ V₂ (ms.map Mut.span)) : applyMuts V₁ l ms = applyMuts V₂ l ms := by
  induction ms generalizing l with
  | nil => rfl
  | cons m ms ih =>
    have h₁ : Agree V₁ V₂ [m.span] := Agree.left (S' := ms.map Mut.span) h
    have h₂ : Agree V₁ V₂ (ms.map Mut.span) := Agree.right (S := [m.span]) h
    simp only [applyMuts, applyMut_congr l m h₁]
    split <;> simp_all

theorem step_congr {cfg : Cfg} {V₁ V₂ : View} (l : Local) (op : Op)
    (h : Agree V₁ V₂ (fp cfg op)) : Local.step V₁ l op = Local.step V₂ l op := by
  cases op with
  | read tbl spec =>
    simp only [Local.step, Local.view, rowsOf_congr (Agree.overlay l.buf h)]
  | sql tbl spec =>
    have h' : Agree V₁ V₂ [⟨tbl, spec⟩] := h.mono fun rk => sqlSpan_covers rk
    simp only [Local.step, Local.view, rowsOf_congr (Agree.overlay l.buf h')]
  | dmlInsert tbl k v =>
    simp only [Local.step, applyMut_congr l ⟨.insert, tbl, k, v⟩ h]
  | dmlUpdate tbl spec v =>
    have h' : Agree V₁ V₂ [⟨tbl, spec⟩] := h.mono fun rk => sqlSpan_covers rk
    simp only [Local.step, Local.view, matched_congr (Agree.overlay l.buf h')]
  | dmlDelete tbl spec =>
    have h' : Agree V₁ V₂ [⟨tbl, spec⟩] := h.mono fun rk => sqlSpan_covers rk
    simp only [Local.step, Local.view, matched_congr (Agree.overlay l.buf h')]
  | _ => rfl

/-- Footprint of a whole program. -/
def progFp (cfg : Cfg) (prog : List Op) : List Span := prog.flatMap (fp cfg)

theorem runProg_congr {cfg : Cfg} {V₁ V₂ : View} (l : Local) (prog : List Op)
    (h : Agree V₁ V₂ (progFp cfg prog)) : runProg V₁ l prog = runProg V₂ l prog := by
  induction prog generalizing l with
  | nil => rfl
  | cons op ops ih =>
    have h₁ : Agree V₁ V₂ (fp cfg op) := Agree.left (S' := progFp cfg ops) h
    have h₂ : Agree V₁ V₂ (progFp cfg ops) := Agree.right (S := fp cfg op) h
    simp only [runProg, step_congr (cfg := cfg) l op h₁]
    split
    · simp only [ih _ h₂]
    · rfl

theorem runTxn_congr {cfg : Cfg} {V₁ V₂ : View} (prog : List Op) (ms : List Mut)
    (h : Agree V₁ V₂ (progFp cfg prog ++ ms.map Mut.span)) :
    runTxn V₁ prog ms = runTxn V₂ prog ms := by
  simp only [runTxn, runProg_congr (cfg := cfg) {} prog h.left]
  split
  · simp only [applyMuts_congr _ ms h.right]
  · rfl

/-! ## Program composition -/

theorem runProg_snoc {V : View} {l : Local} {prog : List Op} {op : Op} {rs : List Res}
    {l' l'' : Local} {r : Res} (h₁ : runProg V l prog = .ok (rs, l'))
    (h₂ : Local.step V l' op = .ok (r, l'')) :
    runProg V l (prog ++ [op]) = .ok (rs ++ [r], l'') := by
  induction prog generalizing l rs with
  | nil =>
    simp only [runProg, Except.ok.injEq, Prod.mk.injEq] at h₁
    obtain ⟨rfl, rfl⟩ := h₁
    simp [runProg, h₂]
  | cons o os ih =>
    simp only [runProg] at h₁
    split at h₁
    · rename_i r₀ l₀ hs
      split at h₁
      · rename_i rs₀ l₁ hp
        simp only [Except.ok.injEq, Prod.mk.injEq] at h₁
        obtain ⟨rfl, rfl⟩ := h₁
        simp [runProg, hs, ih hp]
      · cases h₁
    · cases h₁

theorem runProg_snoc_error {V : View} {l : Local} {prog : List Op} {op : Op} {rs : List Res}
    {l' : Local} {c : Code} (h₁ : runProg V l prog = .ok (rs, l'))
    (h₂ : Local.step V l' op = .error c) :
    runProg V l (prog ++ [op]) = .error c := by
  induction prog generalizing l rs with
  | nil =>
    simp only [runProg, Except.ok.injEq, Prod.mk.injEq] at h₁
    obtain ⟨rfl, rfl⟩ := h₁
    simp [runProg, h₂]
  | cons o os ih =>
    simp only [runProg] at h₁
    split at h₁
    · rename_i r₀ l₀ hs
      split at h₁
      · rename_i rs₀ l₁ hp
        simp only [Except.ok.injEq, Prod.mk.injEq] at h₁
        obtain ⟨rfl, rfl⟩ := h₁
        simp [runProg, hs, ih hp]
      · cases h₁
    · cases h₁

theorem runTxn_of {V : View} {prog : List Op} {ms : List Mut} {rs : List Res} {l l' : Local}
    (h₁ : runProg V {} prog = .ok (rs, l)) (h₂ : applyMuts V l ms = .ok l') :
    runTxn V prog ms = .ok (rs, l') := by
  simp only [runTxn, h₁, h₂]

/-! ## Errors are never `ABORTED`

Transaction-local semantics only produce constraint errors; `ABORTED` comes
from the concurrency control alone. -/

theorem applyMut_not_aborted {V : View} {l : Local} {m : Mut} {c : Code}
    (h : applyMut V l m = .error c) : c ≠ .aborted := by
  unfold applyMut at h
  cases hk : m.kind <;> simp only [hk] at h
  · by_cases h₁ : (l.view V m.rk).isSome = true <;> simp [h₁] at h <;> subst h <;> decide
  · by_cases h₁ : m.rk ∈ l.deleted
    · simp [h₁] at h; subst h; decide
    · by_cases h₂ : (l.view V m.rk).isNone = true <;> simp [h₁, h₂] at h <;> subst h <;> decide
  · simp at h
  · by_cases h₁ : (l.view V m.rk).isSome = true <;> simp [h₁] at h

theorem applyMuts_not_aborted {V : View} {l : Local} {ms : List Mut} {c : Code}
    (h : applyMuts V l ms = .error c) : c ≠ .aborted := by
  induction ms generalizing l with
  | nil => simp [applyMuts] at h
  | cons m ms ih =>
    simp only [applyMuts] at h
    split at h
    · exact ih h
    · rename_i c' hm
      cases h
      exact applyMut_not_aborted hm

theorem step_not_aborted {V : View} {l : Local} {op : Op} {c : Code}
    (h : Local.step V l op = .error c) (hd : op.isData = true) : c ≠ .aborted := by
  cases op <;> simp [Op.isData] at hd <;> simp only [Local.step] at h
  all_goals first
    | (split at h
       · cases h
       · rename_i c' hm; cases h; exact applyMut_not_aborted hm)
    | cases h

theorem step_ok_res {V : View} {l l' : Local} {op : Op} {r : Res}
    (h : Local.step V l op = .ok (r, l')) (c : Code) : r ≠ .err c := by
  intro hr; subst hr
  cases op <;> simp only [Local.step] at h
  all_goals first | (cases h; done) | (split at h <;> cases h)

end TxnSpec
