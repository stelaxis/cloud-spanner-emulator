import TxnSpec.Serial
import TxnSpec.Upstream

/-!
# Upstream: the single-slot emulator is serializable

The key fact (`UTxnInv`): a live RW transaction that has run anything holds the
slot, so no other transaction commits between its first operation and its own
commit; it therefore always reads the latest state.
-/

namespace TxnSpec.Upstream

def Fresh (x : TxnSt) : Prop :=
  x.snap = none ∧ x.loc = {} ∧ x.rset = [] ∧ x.prog = [] ∧ x.results = []

def ROInv (log : List Commit) (x : TxnSt) : Prop :=
  ∃ n, x.snap = some n ∧ n ≤ log.length ∧ x.prog.map (roRead (stateAt log n)) = x.results.map some

def UTxnInv (s : State) (t : Tid) (x : TxnSt) : Prop :=
  match x.status with
  | .idle => Fresh x
  | .active =>
      (x.kind = .rw →
        (x.prog = [] ∧ x.results = [] ∧ x.loc = {}) ∨
        (s.slot = some t ∧ runProg (latest s) {} x.prog = .ok (x.results, x.loc))) ∧
      (x.kind = .ro → ROInv s.log x)
  | _ => True

structure Inv (s : State) : Prop where
  serial : Serializable s.log
  ts : TsFrom 0 s.log
  txn : ∀ t, UTxnInv s t (s.txns t)
  slot : ∀ t, s.slot = some t → (s.txns t).status = .active ∧ (s.txns t).kind = .rw

theorem inv_init : Inv {} where
  serial := trivial
  ts := trivial
  txn _ := by simp only [UTxnInv]; exact ⟨rfl, rfl, rfl, rfl, rfl⟩
  slot _ h := by cases h

@[simp] theorem set_log (s : State) (t : Tid) (x : TxnSt) : (s.set t x).log = s.log := rfl
@[simp] theorem set_slot (s : State) (t : Tid) (x : TxnSt) : (s.set t x).slot = s.slot := rfl
@[simp] theorem set_txns (s : State) (t : Tid) (x : TxnSt) : (s.set t x).txns = upd s.txns t x := rfl

theorem free_iff (s : State) (t : Tid) : free s t = true ↔ s.slot = none ∨ s.slot = some t := by
  unfold free
  cases s.slot with
  | none => simp
  | some u => simp only [beq_iff_eq, reduceCtorEq, false_or, Option.some.injEq]

/-- A started RW transaction holds the slot. -/
theorem holds_slot {s : State} {t : Tid} (h : Inv s) (hs : (s.txns t).status = .active)
    (hk : (s.txns t).kind = .rw) (hp : (s.txns t).prog ≠ []) : s.slot = some t := by
  have hx := h.txn t
  unfold UTxnInv at hx
  rw [hs] at hx
  rcases hx.1 hk with ⟨hp', _⟩ | ⟨hsl, _⟩
  · exact absurd hp' hp
  · exact hsl

/-- The invariant of a transaction that is not active and not idle. -/
theorem utxnInv_final {s : State} {t : Tid} {x : TxnSt} (h₁ : x.status ≠ .idle)
    (h₂ : x.status ≠ .active) : UTxnInv s t x := by
  unfold UTxnInv; split <;> simp_all

/-- Only the slot holder's invariant mentions the slot or the latest state; the
others survive any change that keeps the log, or keeps them off the slot. -/
theorem utxnInv_transfer {s s' : State} {t : Tid} {x : TxnSt} (h : UTxnInv s t x)
    (hro : ∀ {n}, n ≤ s.log.length → n ≤ s'.log.length ∧ stateAt s'.log n = stateAt s.log n)
    (hslot : s.slot ≠ some t) : UTxnInv s' t x := by
  unfold UTxnInv at h ⊢
  split
  · simp_all
  · rename_i hs
    simp only [hs] at h
    refine ⟨fun hk => ?_, fun hk => ?_⟩
    · rcases h.1 hk with h' | ⟨hsl, _⟩
      · exact Or.inl h'
      · exact absurd hsl hslot
    · obtain ⟨n, hn, hle, hm⟩ := h.2 hk
      obtain ⟨hle', he⟩ := hro hle
      exact ⟨n, hn, hle', by rw [he]; exact hm⟩
  · trivial

theorem ro_extend {s : State} (c : Commit) :
    ∀ {n}, n ≤ s.log.length → n ≤ (s.log ++ [c]).length ∧ stateAt (s.log ++ [c]) n = stateAt s.log n := by
  intro n hn; exact ⟨by simp; omega, stateAt_append _ hn⟩

theorem roSnap_le (len : Nat) (at? : Option Nat) : roSnap len at? ≤ len := by
  cases at? <;> simp [roSnap]; omega

theorem inactiveStep_status {x : TxnSt} {op : Op} (h₁ : x.status ≠ .idle) (h₂ : x.status ≠ .active) :
    (inactiveStep x op).2.status ≠ .idle ∧ (inactiveStep x op).2.status ≠ .active := by
  unfold inactiveStep
  split <;> simp_all

theorem utxnInv_keep_log {s s' : State} {t : Tid} {x : TxnSt} (h : UTxnInv s t x)
    (hlog : s'.log = s.log) (hslot : s.slot = some t → s'.slot = some t) : UTxnInv s' t x := by
  have hl : latest s' = latest s := by simp [latest, hlog]
  unfold UTxnInv at h ⊢
  split
  · simp_all
  · rename_i hs
    simp only [hs] at h
    refine ⟨fun hk => ?_, fun hk => ?_⟩
    · rcases h.1 hk with h' | ⟨hsl, hr⟩
      · exact Or.inl h'
      · exact Or.inr ⟨hslot hsl, by rw [hl]; exact hr⟩
    · rw [hlog]; exact h.2 hk
  · trivial

/-- `t`'s state becomes `x'` and the slot becomes `slot'`; the log is unchanged
and no other transaction gains or loses the slot. -/
def mk (s : State) (t : Tid) (x' : TxnSt) (slot' : Option Tid) : State :=
  { log := s.log, txns := upd s.txns t x', slot := slot' }

theorem inv_mk {s : State} (h : Inv s) (t : Tid) (x' : TxnSt) (slot' : Option Tid)
    (hold : ∀ t', t' ≠ t → s.slot = some t' → slot' = some t')
    (hnew : ∀ t', t' ≠ t → slot' = some t' → s.slot = some t')
    (hx : UTxnInv (mk s t x' slot') t x')
    (hsl : slot' = some t → x'.status = .active ∧ x'.kind = .rw) : Inv (mk s t x' slot') where
  serial := h.serial
  ts := h.ts
  txn t' := by
    by_cases e : t' = t
    · subst e; simpa [mk] using hx
    · simp only [mk, upd_other _ _ _ _ e]
      exact utxnInv_keep_log (h.txn t') rfl (hold t' e)
  slot t' hs' := by
    by_cases e : t' = t
    · subst e; simpa [mk] using hsl hs'
    · simp only [mk, upd_other _ _ _ _ e]
      exact h.slot t' (hnew t' e hs')

theorem step_inv (s : State) (st : Step) (h : Inv s) : Inv (step s st).2 := by
  obtain ⟨t, op⟩ := st
  have hx := h.txn t
  have keep : ∀ t', t' ≠ t → s.slot = some t' → s.slot = some t' := fun _ _ e => e
  have keep' : ∀ t', t' ≠ t → s.slot = some t' → s.slot = some t' := fun _ _ e => e
  unfold step
  simp only
  generalize hxs : s.txns t = x at hx
  unfold UTxnInv at hx
  split
  · -- idle: `t` does not hold the slot
    rename_i hst
    simp only [hst] at hx
    obtain ⟨hsn, hl, hrs, hp, hr⟩ := hx
    have hns : s.slot ≠ some t := fun e => by have := (h.slot t e).1; simp_all
    apply inv_mk h t _ s.slot keep keep'
    · cases op <;> simp [beginStep, UTxnInv, ROInv, Fresh, mk, hst, hsn, hl, hrs, hp, hr, roSnap_le]
    · intro e; exact absurd e hns
  · rename_i hst
    simp only [hst] at hx
    split
    · -- RO: never the slot holder
      rename_i hkind
      have hns : s.slot ≠ some t := fun e => by have := (h.slot t e).2; simp_all
      apply inv_mk h t _ s.slot keep keep' _ (fun e => absurd e hns)
      obtain ⟨n, hn, hle, hm⟩ := hx.2 hkind
      unfold roStep
      split
      · rename_i r hr
        simp only [UTxnInv, hst, hkind, reduceCtorEq, false_implies, true_and, mk, true_implies]
        refine ⟨n, hn, hle, ?_⟩
        simp only [hn, Option.getD_some] at hr
        simp [List.map_append, hm, hr]
      · simp only [UTxnInv, hst, hkind, reduceCtorEq, false_implies, true_and, mk, true_implies]
        exact ⟨n, hn, hle, hm⟩
    · -- RW
      rename_i hkind
      have hrw := hx.1 hkind
      have hrun : runProg (latest s) {} x.prog = .ok (x.results, x.loc) := by
        rcases hrw with ⟨hp, hr, hl⟩ | ⟨_, hr⟩
        · simp [hp, hr, hl, runProg]
        · exact hr
      -- If `t` may use the slot, nobody else holds it.
      have others : free s t = true → ∀ t', t' ≠ t → s.slot ≠ some t' := by
        intro hf t' ne e
        rcases (free_iff s t).mp hf with h' | h' <;> rw [h'] at e <;> simp_all
      have release : free s t = true → ∀ t', t' ≠ t → s.slot = some t' → none = some t' :=
        fun hf t' ne e => absurd e (others hf t' ne)
      have noGain : ∀ t', t' ≠ t → (none : Option Tid) = some t' → s.slot = some t' :=
        fun _ _ e => by cases e
      unfold rwStep
      split
      · -- commit
        rename_i ms
        split
        · rename_i hf
          split
          · rename_i l' hm
            refine ⟨?_, ?_, ?_, ?_⟩
            · rw [Serializable, serialFrom_append]
              refine ⟨h.serial, ?_⟩
              rw [← stateAt_length]
              exact runTxn_of hrun hm
            · rw [tsFrom_append]; exact ⟨h.ts, by simp⟩
            · intro t'
              by_cases e : t' = t
              · subst e; simp only [upd_same]; exact utxnInv_final (by simp) (by simp)
              · simp only [upd_other _ _ _ _ e]
                exact utxnInv_transfer (h.txn t') (ro_extend _) (others hf t' e)
            · intro t' e; cases e
          · exact inv_mk h t _ none (release hf) noGain (utxnInv_final (by simp) (by simp))
              (fun e => by cases e)
        · rename_i hf
          simp only [Bool.not_eq_true] at hf
          have hns : s.slot ≠ some t := fun e => by simp [free, e] at hf
          exact inv_mk h t _ s.slot keep keep' (utxnInv_final (by simp) (by simp))
            (fun e => absurd e hns)
      · -- rollback
        split
        · rename_i hf
          exact inv_mk h t _ none (release hf) noGain (utxnInv_final (by simp) (by simp))
            (fun e => by cases e)
        · rename_i hf
          simp only [Bool.not_eq_true] at hf
          have hns : s.slot ≠ some t := fun e => by simp [free, e] at hf
          exact inv_mk h t _ s.slot keep keep' (utxnInv_final (by simp) (by simp))
            (fun e => absurd e hns)
      · -- data op
        split
        · split
          · rename_i hf
            split
            · rename_i r l' hs
              apply inv_mk h t _ (some t) (fun t' ne e => absurd e (others hf t' ne))
                (fun t' ne e => by cases e; exact absurd rfl ne)
              · simp only [UTxnInv, hst, hkind, reduceCtorEq, false_implies, and_true, mk,
                  forall_const]
                right
                refine ⟨by simp, ?_⟩
                exact runProg_snoc hrun hs
              · intro _; exact ⟨hst, hkind⟩
            · exact inv_mk h t _ none (release hf) noGain (utxnInv_final (by simp) (by simp))
                (fun e => by cases e)
          · rename_i hf
            simp only [Bool.not_eq_true] at hf
            have hns : s.slot ≠ some t := fun e => by simp [free, e] at hf
            exact inv_mk h t _ s.slot keep keep' (utxnInv_final (by simp) (by simp))
              (fun e => absurd e hns)
        · exact h
  · -- dead / committed / rolled back: not the slot holder
    rename_i hi ha
    have hns : s.slot ≠ some t := fun e => by have := (h.slot t e).1; simp_all
    obtain ⟨h₁, h₂⟩ := inactiveStep_status (op := op) hi ha
    exact inv_mk h t _ s.slot keep keep' (utxnInv_final h₁ h₂) (fun e => absurd e hns)

theorem run_inv (s : State) (σ : List Step) (h : Inv s) : Inv (run s σ).2 := by
  induction σ generalizing s with
  | nil => exact h
  | cons st sts ih => exact ih _ (step_inv s st h)

/-- **(iv-a) Upstream is serializable**, with strictly increasing commit
timestamps. -/
theorem upstream_serializable (σ : List Step) :
    let log := (run {} σ).2.log
    Serializable log ∧
      serialExec emptyView (log.map fun c => (c.prog, c.muts)) =
        .ok (log.map (·.results), stateAt log log.length) ∧
      log.Pairwise fun a b => a.ts < b.ts := by
  have h := run_inv {} σ inv_init
  exact ⟨h.serial, serialExec_eq h.serial, tsFrom_strict h.ts⟩

end TxnSpec.Upstream
