import TxnSpec.Json

/-! `txnmodel`: runs schedules through the Upstream or Target model.

    txnmodel --model upstream|target [--pushdown] [--batch]

Default: stdin is one schedule, one step per line; prints one result per step.
`--batch`: every stdin line is a whole schedule (a JSON array of steps); prints
one JSON array of results per line and flushes, for the conformance harness. -/

open TxnSpec TxnSpec.Json Lean

def usage : String := "usage: txnmodel --model upstream|target [--pushdown] [--batch]"

def parseArgs : List String → Model → Bool → Bool → Except String (Model × Bool)
  | [], m, pd, b => .ok (match m with
      | .target cfg => .target { cfg with pushdown := pd }
      | m => m, b)
  | "--model" :: "upstream" :: rest, _, pd, b => parseArgs rest .upstream pd b
  | "--model" :: "target" :: rest, _, pd, b => parseArgs rest (.target {}) pd b
  | "--pushdown" :: rest, m, _, b => parseArgs rest m true b
  | "--batch" :: rest, m, pd, _ => parseArgs rest m pd true
  | a :: _, _, _, _ => .error s!"unknown argument {a}\n{usage}"

def main (args : List String) : IO UInt32 := do
  let (model, batch) ← match parseArgs args .upstream false false with
    | .ok r => pure r
    | .error e => IO.eprintln e; return 2
  let stdin ← IO.getStdin
  let stdout ← IO.getStdout
  if batch then
    repeat
      let line ← stdin.getLine
      if line.isEmpty then break
      if line.all Char.isWhitespace then continue
      match runBatchLine model line with
      | .ok out => stdout.putStrLn out; stdout.flush
      | .error e => IO.eprintln s!"txnmodel: bad schedule: {e}"; return 1
    return 0
  else
    let input ← stdin.readToEnd
    let mut steps : Array Step := #[]
    for line in input.splitOn "\n" do
      if line.all Char.isWhitespace then continue
      match Json.parse line >>= stepOf with
      | .ok st => steps := steps.push st
      | .error e => IO.eprintln s!"txnmodel: bad step: {e}"; return 1
    let res := runModel model steps.toList
    for (st, r, i) in steps.toList.zip (res.zip (List.range res.length)) do
      stdout.putStrLn (Json.mkObj [("step", toJson i), ("txn", toJson st.txn), ("res", resJson r)]).compress
    return 0
