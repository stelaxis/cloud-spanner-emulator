import Lean.Data.Json
import TxnSpec.Upstream
import TxnSpec.Target

/-!
# Schedule JSON codec for `txnmodel`

See `verification/README.md` for the format.
-/

namespace TxnSpec.Json

open Lean (Json ToJson toJson)

def tableOf (s : String) : Except String Nat :=
  match s with
  | "A" => .ok 0
  | "B" => .ok 1
  | _ => .error s!"unknown table {s}"

def keyOf (j : Json) : Except String Key := do
  let k ← j.getNat?
  if k < keyBound then pure k else throw s!"key {k} outside 0..{keyBound - 1}"

def specOf (j : Json) : Except String KeySpec := do
  if j == Json.str "all" then return .all
  if let .ok k := j.getObjVal? "point" then return .point (← keyOf k)
  let lo ← keyOf (← j.getObjVal? "lo")
  let hi ← keyOf (← j.getObjVal? "hi")
  let li ← (← j.getObjVal? "lo_incl").getBool?
  let hc ← (← j.getObjVal? "hi_incl").getBool?
  return .range lo li hi hc

def mutKindOf (s : String) : Except String MutKind :=
  match s with
  | "insert" => .ok .insert
  | "update" => .ok .update
  | "upsert" => .ok .upsert
  | "delete" => .ok .delete
  | _ => .error s!"unknown mutation kind {s}"

def mutOf (j : Json) : Except String Mut := do
  let kind ← mutKindOf (← (← j.getObjVal? "kind").getStr?)
  let tbl ← tableOf (← (← j.getObjVal? "table").getStr?)
  let key ← keyOf (← j.getObjVal? "key")
  let val ← match j.getObjVal? "val" with
    | .ok v => v.getInt?
    | .error _ => pure 0
  return { kind, tbl, key, val }

def stepOf (j : Json) : Except String Step := do
  let txn ← (← j.getObjVal? "txn").getNat?
  let opName ← (← j.getObjVal? "op").getStr?
  let args := (j.getObjVal? "args").toOption.getD (Json.mkObj [])
  let arg (k : String) : Except String Json := args.getObjVal? k
  let tbl : Except String Nat := do tableOf (← (← arg "table").getStr?)
  let op ← match opName with
    | "begin_rw" => pure Op.beginRW
    | "begin_ro" => pure (Op.beginRO ((arg "at").toOption.bind fun a => a.getNat?.toOption))
    | "read" => pure (Op.read (← tbl) (← specOf (← arg "keys")))
    | "sql" => pure (Op.sql (← tbl) (← specOf (← arg "keys")))
    | "dml_insert" => pure (Op.dmlInsert (← tbl) (← keyOf (← arg "key")) (← (← arg "val").getInt?))
    | "dml_update" => pure (Op.dmlUpdate (← tbl) (← specOf (← arg "keys")) (← (← arg "val").getInt?))
    | "dml_delete" => pure (Op.dmlDelete (← tbl) (← specOf (← arg "keys")))
    | "commit" =>
        let ms ← match arg "mutations" with
          | .ok a => (← a.getArr?).toList.mapM mutOf
          | .error _ => pure []
        pure (Op.commit ms)
    | "rollback" => pure Op.rollback
    | o => throw s!"unknown op {o}"
  return { txn, op }

def codeName : Code → String
  | .aborted => "ABORTED"
  | .alreadyExists => "ALREADY_EXISTS"
  | .notFound => "NOT_FOUND"
  | .invalidArgument => "INVALID_ARGUMENT"
  | .failedPrecondition => "FAILED_PRECONDITION"

def resJson : Res → Json
  | .ok => Json.str "ok"
  | .rows rs => Json.mkObj [("rows", Json.arr (rs.map fun (k, v) => Json.arr #[toJson k, toJson v]).toArray)]
  | .count n => Json.mkObj [("count", toJson n)]
  | .committed ts => Json.mkObj [("committed", toJson ts)]
  | .err c => Json.mkObj [("error", Json.str (codeName c))]

inductive Model where
  | upstream
  | target (cfg : Cfg)

def runModel : Model → List Step → List Res
  | .upstream, sts => (Upstream.run {} sts).1
  | .target cfg, sts => (Target.run cfg {} sts).1

/-- One schedule (JSON array of steps) → JSON array of results. -/
def runBatchLine (m : Model) (line : String) : Except String String := do
  let j ← Json.parse line
  let sts ← (← j.getArr?).toList.mapM stepOf
  return (Json.arr ((runModel m sts).map resJson).toArray).compress

end TxnSpec.Json
