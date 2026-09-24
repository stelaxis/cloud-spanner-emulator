import TxnSpec.Json
import TxnSpec.PersistenceComposition

namespace TxnSpec.Json
open Lean (Json ToJson toJson)
open Persistence

/-- Crash schedules have their own envelope so L1's transaction algebra and
proofs do not acquire fictitious transaction operations. -/
inductive ScheduleStep where
  | call (st : Step)
  | crash (commit : Option Step) (durable terminal : Bool)
  | restart

def scheduleStepOf (j : Json) : Except String ScheduleStep := do
  let name ← (← j.getObjVal? "op").getStr?
  if name == "restart" then return .restart
  if name == "crash" then
    let args := (j.getObjVal? "args").toOption.getD (Json.mkObj [])
    let during := ((args.getObjVal? "during_commit").toOption.bind fun x => x.getBool?.toOption).getD false
    let durable := ((args.getObjVal? "durable").toOption.bind fun x => x.getBool?.toOption).getD false
    let terminal := ((args.getObjVal? "terminal").toOption.bind fun x => x.getBool?.toOption).getD false
    if during then
      let txn ← j.getObjVal? "txn"
      let st ← stepOf (Json.mkObj [("txn", txn), ("op", Json.str "commit"), ("args", args)])
      return .crash (some st) durable terminal
    return .crash none false false
  return .call (← stepOf j)

structure RestartState where
  upstream : Upstream.State := {}
  target : Target.State := {}
  running : Bool := true
  boundary : Nat := 0
  acknowledged : Nat := 0
  ackTimes : List Nat := []

def modelStep (m : Model) (persistent : Bool) (s : RestartState) (st : ScheduleStep) : Res × RestartState :=
  match st with
  | .crash pending durable terminal =>
    if (persistent && durable) || terminal then
      match pending with
      | none => (.ok, { s with running := false })
      | some call =>
        let (r, s) := match m with
          | .upstream => let (r, u) := Upstream.step s.upstream call; (r, { s with upstream := u })
          | .target cfg => let (r, t) := Target.step cfg s.target call; (r, { s with target := t })
        match r with
        | .committed n => (if terminal then .committed n else .ok, { s with running := false })
        | _ => (r, { s with running := false })
    else (.ok, { s with running := false })
  | .restart =>
    if persistent then
      (.ok, { s with upstream := { log := s.upstream.log }, target := { log := s.target.log }, running := true, boundary := s.acknowledged + 1 })
    else (.ok, {})
  | .call call =>
    if !s.running then (.err .failedPrecondition, s)
    else if (match call.op with | .beginRO (some n) => decide (min n s.acknowledged < s.boundary) | _ => false) then
      (.err .failedPrecondition, s)
    else
      let call := match call.op with
        | .beginRO (some n) => { call with op := .beginRO (some (s.ackTimes[n - 1]?.getD n)) }
        | _ => call
      let (r, s) := match m with
        | .upstream => let (r, u) := Upstream.step s.upstream call; (r, { s with upstream := u })
        | .target cfg => let (r, t) := Target.step cfg s.target call; (r, { s with target := t })
      match r with
      | .committed ts =>
        if s.ackTimes.contains ts then (.committed (s.ackTimes.findIdx (· == ts) + 1), s)
        else (.committed (s.acknowledged + 1), { s with acknowledged := s.acknowledged + 1, ackTimes := s.ackTimes ++ [ts] })
      | _ => (r, s)

def runRestartState (m : Model) (persistent : Bool) (s : RestartState) :
    List ScheduleStep → List Res × RestartState
  | [] => ([], s)
  | st :: sts =>
      let (r, t) := modelStep m persistent s st
      let (rs, u) := runRestartState m persistent t sts
      (r :: rs, u)

def runRestartModel (m : Model) (persistent : Bool) (steps : List ScheduleStep) : List Res :=
  (runRestartState m persistent {} steps).1

theorem modelStep_target_across (cfg : Cfg) (persistent : Bool) (s : RestartState)
    (st : ScheduleStep) (h : AcrossRestarts cfg s.target) :
    AcrossRestarts cfg (modelStep (.target cfg) persistent s st).2.target := by
  cases st with
  | crash pending durable terminal =>
      simp only [modelStep]
      split
      · cases pending with
        | none => exact h
        | some call =>
            have hh := AcrossRestarts.call call h
            dsimp only
            split <;> exact hh
      · exact h
  | restart =>
      simp only [modelStep]
      split
      · exact AcrossRestarts.crash h
      · exact AcrossRestarts.initial
  | call call =>
      simp only [modelStep]
      repeat' first | exact h | exact AcrossRestarts.call _ h | split

theorem runRestartState_target_across (cfg : Cfg) (persistent : Bool)
    (steps : List ScheduleStep) (s : RestartState) (h : AcrossRestarts cfg s.target) :
    AcrossRestarts cfg (runRestartState (.target cfg) persistent s steps).2.target := by
  induction steps generalizing s with
  | nil => exact h
  | cons st sts ih => exact ih _ (modelStep_target_across cfg persistent s st h)

/-- Every executable-oracle prefix has the proved Target invariant. -/
theorem runRestartModel_target_inv (cfg : Cfg) (hk : cfg.keysOnlyReadSet = false)
    (persistent : Bool) (steps : List ScheduleStep) :
    AcrossRestarts cfg (runRestartState (.target cfg) persistent {} steps).2.target ∧
    Target.Inv cfg (runRestartState (.target cfg) persistent {} steps).2.target := by
  have hh := runRestartState_target_across cfg persistent steps {} .initial
  exact ⟨hh, across_restarts_inv cfg hk hh⟩

#print axioms runRestartModel_target_inv

/-- One schedule (JSON array of steps) → JSON array of results. -/
def runRestartBatchLine (m : Model) (persistent : Bool) (line : String) : Except String String := do
  let j ← Json.parse line
  let sts ← (← j.getArr?).toList.mapM scheduleStepOf
  return (Json.arr ((runRestartModel m persistent sts).map resJson).toArray).compress

end TxnSpec.Json
