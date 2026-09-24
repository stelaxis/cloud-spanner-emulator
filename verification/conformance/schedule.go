package main

import (
	"encoding/json"
	"fmt"
	"strings"
)

// Keys is a key set over one table: a point, a range, or the whole table.
type Keys struct {
	All    bool
	Point  *int
	Lo, Hi int
	LoIncl bool
	HiIncl bool
}

func (k Keys) MarshalJSON() ([]byte, error) {
	switch {
	case k.All:
		return json.Marshal("all")
	case k.Point != nil:
		return json.Marshal(map[string]int{"point": *k.Point})
	default:
		return json.Marshal(map[string]any{"lo": k.Lo, "lo_incl": k.LoIncl, "hi": k.Hi, "hi_incl": k.HiIncl})
	}
}

func (k Keys) String() string {
	switch {
	case k.All:
		return "all"
	case k.Point != nil:
		return fmt.Sprintf("{%d}", *k.Point)
	default:
		l, r := "(", ")"
		if k.LoIncl {
			l = "["
		}
		if k.HiIncl {
			r = "]"
		}
		return fmt.Sprintf("%s%d,%d%s", l, k.Lo, k.Hi, r)
	}
}

// Mut is a mutation sent with Commit.
type Mut struct {
	Kind  string `json:"kind"` // insert | update | upsert | delete
	Table string `json:"table"`
	Key   int    `json:"key"`
	Val   int64  `json:"val"`
}

// Step is one call of one transaction. Txn 0 is the setup transaction.
type Step struct {
	Txn   int
	Op    string // begin_rw begin_ro read sql dml_insert dml_update dml_delete commit rollback
	Table string
	Keys  *Keys
	Key   int
	Val   int64
	At    *int // begin_ro: exact timestamp of the n-th commit (nil = strong)
	Muts  []Mut
}

func (s Step) MarshalJSON() ([]byte, error) {
	args := map[string]any{}
	switch s.Op {
	case "begin_ro":
		if s.At != nil {
			args["at"] = *s.At
		}
	case "read", "sql", "dml_delete":
		args["table"], args["keys"] = s.Table, s.Keys
	case "dml_update":
		args["table"], args["keys"], args["val"] = s.Table, s.Keys, s.Val
	case "dml_insert":
		args["table"], args["key"], args["val"] = s.Table, s.Key, s.Val
	case "commit":
		if s.Muts == nil {
			args["mutations"] = []Mut{}
		} else {
			args["mutations"] = s.Muts
		}
	}
	return json.Marshal(map[string]any{"txn": s.Txn, "op": s.Op, "args": args})
}

func (s Step) String() string {
	switch s.Op {
	case "begin_ro":
		if s.At != nil {
			return fmt.Sprintf("T%d begin_ro at=%d", s.Txn, *s.At)
		}
		return fmt.Sprintf("T%d begin_ro strong", s.Txn)
	case "read", "sql", "dml_delete":
		return fmt.Sprintf("T%d %s %s%s", s.Txn, s.Op, s.Table, s.Keys)
	case "dml_update":
		return fmt.Sprintf("T%d %s %s%s := %d", s.Txn, s.Op, s.Table, s.Keys, s.Val)
	case "dml_insert":
		return fmt.Sprintf("T%d %s %s{%d} := %d", s.Txn, s.Op, s.Table, s.Key, s.Val)
	case "commit":
		var ms []string
		for _, m := range s.Muts {
			ms = append(ms, fmt.Sprintf("%s %s{%d}=%d", m.Kind, m.Table, m.Key, m.Val))
		}
		return fmt.Sprintf("T%d commit [%s]", s.Txn, strings.Join(ms, ", "))
	default:
		return fmt.Sprintf("T%d %s", s.Txn, s.Op)
	}
}

// Outcome is a canonical step result, comparable between emulator and model.
type Outcome string

func okOutcome() Outcome               { return "ok" }
func countOutcome(n int64) Outcome     { return Outcome(fmt.Sprintf("count:%d", n)) }
func committedOutcome(i int) Outcome   { return Outcome(fmt.Sprintf("committed:%d", i)) }
func errorOutcome(code string) Outcome { return Outcome("error:" + code) }
func rowsOutcome(rows [][2]int64) Outcome {
	var b strings.Builder
	b.WriteString("rows:")
	for i, r := range rows {
		if i > 0 {
			b.WriteString(",")
		}
		fmt.Fprintf(&b, "%d=%d", r[0], r[1])
	}
	return Outcome(b.String())
}

// Kind is the outcome without its payload, for divergence summaries.
func (o Outcome) Kind() string {
	s := string(o)
	if strings.HasPrefix(s, "error:") {
		return s
	}
	if i := strings.IndexByte(s, ':'); i >= 0 {
		return s[:i]
	}
	return s
}

// parseModelResult converts one txnmodel result to an Outcome.
func parseModelResult(raw json.RawMessage) (Outcome, error) {
	var s string
	if json.Unmarshal(raw, &s) == nil {
		return Outcome(s), nil
	}
	var m struct {
		Rows      *[][2]int64 `json:"rows"`
		Count     *int64      `json:"count"`
		Committed *int        `json:"committed"`
		Error     *string     `json:"error"`
	}
	if err := json.Unmarshal(raw, &m); err != nil {
		return "", err
	}
	switch {
	case m.Rows != nil:
		return rowsOutcome(*m.Rows), nil
	case m.Count != nil:
		return countOutcome(*m.Count), nil
	case m.Committed != nil:
		return committedOutcome(*m.Committed), nil
	case m.Error != nil:
		return errorOutcome(*m.Error), nil
	}
	return "", fmt.Errorf("unrecognised model result %s", raw)
}

func (k *Keys) UnmarshalJSON(b []byte) error {
	var s string
	if json.Unmarshal(b, &s) == nil {
		if s != "all" {
			return fmt.Errorf("bad key set %q", s)
		}
		*k = Keys{All: true}
		return nil
	}
	var m struct {
		Point  *int `json:"point"`
		Lo     int  `json:"lo"`
		Hi     int  `json:"hi"`
		LoIncl bool `json:"lo_incl"`
		HiIncl bool `json:"hi_incl"`
	}
	if err := json.Unmarshal(b, &m); err != nil {
		return err
	}
	*k = Keys{Point: m.Point, Lo: m.Lo, Hi: m.Hi, LoIncl: m.LoIncl, HiIncl: m.HiIncl}
	return nil
}

func (s *Step) UnmarshalJSON(b []byte) error {
	var raw struct {
		Txn  int    `json:"txn"`
		Op   string `json:"op"`
		Args struct {
			Table     string `json:"table"`
			Keys      *Keys  `json:"keys"`
			Key       int    `json:"key"`
			Val       int64  `json:"val"`
			At        *int   `json:"at"`
			Mutations []Mut  `json:"mutations"`
		} `json:"args"`
	}
	if err := json.Unmarshal(b, &raw); err != nil {
		return err
	}
	a := raw.Args
	*s = Step{Txn: raw.Txn, Op: raw.Op, Table: a.Table, Keys: a.Keys, Key: a.Key, Val: a.Val, At: a.At, Muts: a.Mutations}
	return nil
}
