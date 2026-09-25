import TxnSpec.RestartJson

/-! `txnmodel`: runs schedules through the Upstream or Target model.

    txnmodel --model upstream|target [--pushdown] [--batch] [--persistence no-persistence|persistent]

Default: stdin is one schedule, one step per line; prints one result per step.
`--batch`: every stdin line is a whole schedule (a JSON array of steps); prints
one JSON array of results per line and flushes, for the conformance harness. -/

open TxnSpec TxnSpec.Json Lean

def usage : String := "usage: txnmodel --model upstream|target [--pushdown] [--batch] [--persistence no-persistence|persistent]"

def parseArgs : List String → Model → Bool → Bool → Bool → Except String (Model × Bool × Bool)
  | [], m, pd, b, p => .ok (match m with
      | .target cfg => .target { cfg with pushdown := pd }
      | m => m, b, p)
  | "--model" :: "upstream" :: rest, _, pd, b, p => parseArgs rest .upstream pd b p
  | "--model" :: "target" :: rest, _, pd, b, p => parseArgs rest (.target {}) pd b p
  | "--pushdown" :: rest, m, _, b, p => parseArgs rest m true b p
  | "--batch" :: rest, m, pd, _, p => parseArgs rest m pd true p
  | "--persistence" :: "persistent" :: rest, m, pd, b, _ => parseArgs rest m pd b true
  | "--persistence" :: "no-persistence" :: rest, m, pd, b, _ => parseArgs rest m pd b false
  | a :: _, _, _, _, _ => .error s!"unknown argument {a}\n{usage}"

def main (args : List String) : IO UInt32 := do
  let (model, batch, persistent) ← match parseArgs args .upstream false false false with
    | .ok r => pure r
    | .error e => IO.eprintln e; return 2
  let stdin ← IO.getStdin
  let stdout ← IO.getStdout
  if batch then
    repeat
      let line ← stdin.getLine
      if line.isEmpty then break
      if line.all Char.isWhitespace then continue
      match runRestartBatchLine model persistent line with
      | .ok out => stdout.putStrLn out; stdout.flush
      | .error e => IO.eprintln s!"txnmodel: bad schedule: {e}"; return 1
    return 0
  else
    let input ← stdin.readToEnd
    let mut steps : Array (Json × ScheduleStep) := #[]
    for line in input.splitOn "\n" do
      if line.all Char.isWhitespace then continue
      match Json.parse line >>= fun j => do pure (j, ← scheduleStepOf j) with
      | .ok st => steps := steps.push st
      | .error e => IO.eprintln s!"txnmodel: bad step: {e}"; return 1
    let res := runRestartModel model persistent (steps.toList.map Prod.snd)
    for (st, r, i) in steps.toList.zip (res.zip (List.range res.length)) do
      stdout.putStrLn (Json.mkObj [("step", toJson i), ("txn", (st.1.getObjVal? "txn").toOption.getD (toJson (0 : Nat))), ("res", resJson r)]).compress
    return 0
