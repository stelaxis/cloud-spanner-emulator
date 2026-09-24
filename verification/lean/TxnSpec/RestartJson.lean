import TxnSpec.Json

namespace TxnSpec.Json
open Lean (Json ToJson toJson)

/-- Crash schedules have their own envelope so L1's transaction algebra and
proofs do not acquire fictitious transaction operations. -/
inductive ScheduleStep where
  | call (st : Step)
  | crash (commit : Option Step) (durable : Bool)
  | restart

def scheduleStepOf (j : Json) : Except String ScheduleStep := do
  let name ← (← j.getObjVal? "op").getStr?
  if name == "restart" then return .restart
  if name == "crash" then
    let args := (j.getObjVal? "args").toOption.getD (Json.mkObj [])
    let during := ((args.getObjVal? "during_commit").toOption.bind fun x => x.getBool?.toOption).getD false
    let durable := ((args.getObjVal? "durable").toOption.bind fun x => x.getBool?.toOption).getD false
    if during then
      let txn ← j.getObjVal? "txn"
      let st ← stepOf (Json.mkObj [("txn", txn), ("op", Json.str "commit"), ("args", args)])
      return .crash (some st) durable
    return .crash none false
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
  | .crash pending durable =>
    if persistent && durable then
      match pending with
      | none => (.ok, { s with running := false })
      | some call =>
        let (r, s) := match m with
          | .upstream => let (r, u) := Upstream.step s.upstream call; (r, { s with upstream := u })
          | .target cfg => let (r, t) := Target.step cfg s.target call; (r, { s with target := t })
        match r with
        | .committed _ => (.ok, { s with running := false })
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

def runRestartModel (m : Model) (persistent : Bool) (steps : List ScheduleStep) : List Res :=
  (steps.foldl (fun (rs, s) st => let (r, s') := modelStep m persistent s st; (rs ++ [r], s')) ([], {})).1

/-- One schedule (JSON array of steps) → JSON array of results. -/
def runRestartBatchLine (m : Model) (persistent : Bool) (line : String) : Except String String := do
  let j ← Json.parse line
  let sts ← (← j.getArr?).toList.mapM scheduleStepOf
  return (Json.arr ((runRestartModel m persistent sts).map resJson).toArray).compress

end TxnSpec.Json
