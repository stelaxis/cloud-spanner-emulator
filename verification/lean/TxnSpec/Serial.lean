import TxnSpec.Frame

/-!
# Serializability, and facts about the commit log

A committed transaction is *valid* against a view if running its program and
mutations atomically on that view reproduces exactly the results it returned
and the writes it committed. A log is *serializable* if each commit is valid
against the state left by all earlier commits — i.e. the history equals the
serial execution in commit-timestamp order (`serialExec_eq`).
-/

namespace TxnSpec

def Commit.Valid (V : View) (c : Commit) : Prop :=
  runTxn V c.prog c.muts = .ok (c.results, c.final)

def SerialFrom : View → List Commit → Prop
  | _, [] => True
  | V, c :: cs => c.Valid V ∧ SerialFrom (overlay c.writes V) cs

def Serializable (log : List Commit) : Prop := SerialFrom emptyView log

/-- Serial execution: run each program atomically, in order, on the state the
previous ones left. Returns every transaction's results and the final state. -/
def serialExec : View → List (List Op × List Mut) → Except Code (List (List Res) × View)
  | V, [] => .ok ([], V)
  | V, (prog, ms) :: rest => match runTxn V prog ms with
    | .ok (rs, l) => match serialExec (overlay l.buf V) rest with
      | .ok (rss, V') => .ok (rs :: rss, V')
      | .error c => .error c
    | .error c => .error c

theorem applyLog_append (V : View) (xs ys : List Commit) :
    applyLog V (xs ++ ys) = applyLog (applyLog V xs) ys := by
  induction xs generalizing V with
  | nil => rfl
  | cons c cs ih => exact ih _

theorem serialFrom_append (V : View) (cs : List Commit) (c : Commit) :
    SerialFrom V (cs ++ [c]) ↔ SerialFrom V cs ∧ c.Valid (applyLog V cs) := by
  induction cs generalizing V with
  | nil => simp [SerialFrom, applyLog]
  | cons d ds ih =>
    simp only [List.cons_append, SerialFrom, applyLog, ih]
    exact and_assoc.symm

theorem serialExec_of_serialFrom (V : View) (cs : List Commit) (h : SerialFrom V cs) :
    serialExec V (cs.map fun c => (c.prog, c.muts)) = .ok (cs.map (·.results), applyLog V cs) := by
  induction cs generalizing V with
  | nil => rfl
  | cons c cs ih =>
    obtain ⟨hv, hr⟩ := h
    simp only [List.map_cons, serialExec, applyLog]
    rw [Commit.Valid] at hv
    rw [hv]
    simp only [Commit.writes] at ih hr ⊢
    rw [ih _ hr]

theorem applyLog_take_length (V : View) (log : List Commit) :
    applyLog V (log.take log.length) = applyLog V log := by
  rw [List.take_length]

/-- **Serial equivalence.** A serializable log's transactions return exactly
what they would return run one at a time in commit order from the empty
database, and the store equals the serial result. -/
theorem serialExec_eq {log : List Commit} (h : Serializable log) :
    serialExec emptyView (log.map fun c => (c.prog, c.muts)) =
      .ok (log.map (·.results), stateAt log log.length) := by
  rw [serialExec_of_serialFrom _ _ h, stateAt, applyLog_take_length]

/-! ## Snapshots are stable as the log grows -/

theorem stateAt_append {log : List Commit} {n : Nat} (more : List Commit) (h : n ≤ log.length) :
    stateAt (log ++ more) n = stateAt log n := by
  simp only [stateAt, List.take_append_of_le_length h]

theorem stateAt_length (log : List Commit) : stateAt log log.length = applyLog emptyView log := by
  simp [stateAt]

/-- A commit writing nothing in `S` leaves `S` unchanged. -/
theorem lookupW_none_of_not_conflicts {S : List Span} {ws : Writes} {rk : RowKey}
    (h : (ws.any fun w => covered S w.1) = false) (hc : covered S rk = true) :
    lookupW ws rk = none := by
  induction ws with
  | nil => rfl
  | cons w ws ih =>
    obtain ⟨k, v⟩ := w
    simp only [List.any_cons, Bool.or_eq_false_iff] at h
    simp only [lookupW]
    have hk : k ≠ rk := by
      intro e; subst e; simp_all
    simp [hk, ih h.2]

theorem agree_applyLog {S : List Span} (V : View) (cs : List Commit)
    (h : ∀ c ∈ cs, conflicts S c = false) : Agree V (applyLog V cs) S := by
  induction cs generalizing V with
  | nil => intro rk _; rfl
  | cons c cs ih =>
    have h₁ : Agree V (overlay c.writes V) S := by
      intro rk hc
      simp only [overlay, lookupW_none_of_not_conflicts (h c List.mem_cons_self) hc, Option.getD_none]
    exact h₁.trans (ih _ fun d hd => h d (List.mem_cons_of_mem c hd))

/-- **Validation soundness.** If no commit after snapshot `n` wrote a row in
`S`, the snapshot and the latest state agree on `S`. -/
theorem agree_of_not_stale {log : List Commit} {n : Nat} {S : List Span}
    (h : ((log.drop n).any (conflicts S)) = false) :
    Agree (stateAt log n) (stateAt log log.length) S := by
  rw [stateAt_length, stateAt]
  have e : applyLog emptyView log = applyLog (applyLog emptyView (log.take n)) (log.drop n) := by
    rw [← applyLog_append, List.take_append_drop]
  rw [e]
  apply agree_applyLog
  intro c hc
  simp only [List.any_eq_false] at h
  simpa using h c hc

/-! ## Timestamps -/

/-- The `i`-th commit (0-based) after `n` earlier ones has timestamp `n + i + 1`. -/
def TsFrom : Nat → List Commit → Prop
  | _, [] => True
  | n, c :: cs => c.ts = n + 1 ∧ TsFrom (n + 1) cs

theorem tsFrom_append (n : Nat) (cs : List Commit) (c : Commit) :
    TsFrom n (cs ++ [c]) ↔ TsFrom n cs ∧ c.ts = n + cs.length + 1 := by
  induction cs generalizing n with
  | nil => simp [TsFrom]
  | cons d ds ih =>
    simp only [List.cons_append, TsFrom, ih, List.length_cons]
    constructor
    · rintro ⟨h₁, h₂, h₃⟩; exact ⟨⟨h₁, h₂⟩, by omega⟩
    · rintro ⟨⟨h₁, h₂⟩, h₃⟩; exact ⟨h₁, h₂, by omega⟩

theorem tsFrom_gt {n : Nat} {cs : List Commit} (h : TsFrom n cs) : ∀ c ∈ cs, n < c.ts := by
  induction cs generalizing n with
  | nil => simp
  | cons d ds ih =>
    obtain ⟨h₁, h₂⟩ := h
    intro c hc
    rcases List.mem_cons.mp hc with rfl | hc
    · omega
    · have := ih h₂ c hc; omega

/-- **Commit timestamps strictly increase** along the log. -/
theorem tsFrom_strict {n : Nat} {cs : List Commit} (h : TsFrom n cs) :
    cs.Pairwise fun a b => a.ts < b.ts := by
  induction cs generalizing n with
  | nil => exact List.Pairwise.nil
  | cons d ds ih =>
    obtain ⟨h₁, h₂⟩ := h
    refine List.Pairwise.cons (fun c hc => ?_) (ih h₂)
    have := tsFrom_gt h₂ c hc; omega

/-- The commits visible at snapshot `m` are exactly those with `ts ≤ m`. -/
theorem take_eq_filter_ts {n : Nat} {cs : List Commit} (h : TsFrom n cs) (m : Nat) :
    cs.take (m - n) = cs.filter fun c => decide (c.ts ≤ m) := by
  induction cs generalizing n with
  | nil => simp
  | cons d ds ih =>
    obtain ⟨h₁, h₂⟩ := h
    by_cases hm : n + 1 ≤ m
    · have e : m - n = (m - (n + 1)) + 1 := by omega
      rw [e, List.take_succ_cons, ih h₂, List.filter_cons]
      simp [h₁, hm]
    · have e : m - n = 0 := by omega
      rw [e, List.take_zero, List.filter_cons]
      have hd : ¬ d.ts ≤ m := by omega
      simp only [hd, decide_false, Bool.false_eq_true, ite_false]
      symm
      rw [List.filter_eq_nil_iff]
      intro c hc
      have := tsFrom_gt h₂ c hc
      simp; omega

theorem stateAt_eq_filter {log : List Commit} (h : TsFrom 0 log) (m : Nat) :
    stateAt log m = applyLog emptyView (log.filter fun c => decide (c.ts ≤ m)) := by
  rw [stateAt, ← take_eq_filter_ts h m, Nat.sub_zero]

end TxnSpec
